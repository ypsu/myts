/*
 * Copyright (C) 2010 Luigi Rizzo, Universita' di Pisa
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * $Id: myts.c 7972 2010-12-05 07:31:05Z luigi $

Framework for event based programming.
Each application supplies a descriptor with init, parse, start
routine and a pointer to app-specific data.
Each application is then expected to record handlers that are called before
and after a select() to define which descriptors to poll, and when
a timeout is due.

 */

#include "myts.h"
#include <sys/wait.h>

int verbose;
struct my_args __me;

/* the list of applications to use */
extern struct app lpad;
struct app *all_apps[] = {
	&lpad,
	NULL,
};

/* add a millisecond value to a timer */
void timeradd_ms(const struct timeval *src, int ms, struct timeval *dst)
{
        dst->tv_usec = src->tv_usec + (ms*1000);
        dst->tv_sec = src->tv_sec + dst->tv_usec / 1000000;
        dst->tv_usec %= 1000000;
}
               
/* set dst to the min of the two -- if cur is unset, ignore */
void timersetmin(struct timeval *dst, const struct timeval *cur)
{       
        if (timerisset(cur) && timercmp(cur, dst, <))
                *dst = *cur;
}

/* returns true if dst set and <= 'now' */
int timerdue(const struct timeval *dst, const struct timeval *now)
{       
        return (timerisset(dst) && timercmp(dst, now, <=));
}

/*
 * Generic session creation routine.
 * size is the size of the descriptor, fd is the main file descriptor
 * on which to work (ignored if -2, causes an error if -1),
 * cb is the callback function and arg a session-specific argument.
 * NB: the new session is stored in a temporary list, otherwise
 * it might interfere with the scanning of the main list.
 * Lists are merged at the beginning of each mainloop.
 */
void *new_sess(int size, int fd, cb_fn cb, void *arg)
{
    struct sess *s;

    if (fd != -2) { /* ignore fd in case -2 */
	if (fd < 0)
	    return NULL;
	fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    s = calloc(1, size);
    if (!s) {
	close(fd);
	DBG(0, "alloc failed\n");
	return NULL;
    }
    s->cb = cb;
    s->arg = arg;
    s->fd = fd;
    s->next = __me.tmp_sess;
    __me.tmp_sess = s;
    return s;
}

/*
 * Main loop implementing connection handling
 */
int mainloop(struct my_args *me)
{

    for (;;) {
	int n;
	struct sess *s, *nexts, **ps;
	fd_set r, w;
	struct cb_args a = {
		.maxfd = 0,
		.r = &r, .w = &w,
		.run = 0 /* prepare select */
	};

	FD_ZERO(&r);
	FD_ZERO(&w);
	gettimeofday(&a.now, NULL);
	a.due = a.now;
	a.due.tv_sec += 1000;
	/* prepare for select */
	if (me->tmp_sess) {
	    for (n = 1, s = me->tmp_sess; s->next; s = s->next)
		n++;
	    DBG(2, "merging %d new sessions\n", n);
	    s->next = me->sess;
	    me->sess = me->tmp_sess;
	    me->tmp_sess = NULL;
	}
	for (n = 0, s = me->sess; s; s = s->next) {
	    n++;
	    me->cur = s;
	    me->app = s->app;
	    if (s->cb(s, &a) && a.maxfd < s->fd)
		a.maxfd = s->fd;
	}
	gettimeofday(&a.now, NULL);
	a.due.tv_sec -= a.now.tv_sec;
	a.due.tv_usec -= a.now.tv_usec;
	if (a.due.tv_usec < 0) {
		a.due.tv_usec += 1000000;
		a.due.tv_sec--;
	}
	if (a.due.tv_sec > 100)
		a.due.tv_sec = 100;
	if (a.due.tv_sec < 0)
		a.due.tv_sec = a.due.tv_usec = 0;
	DBG(2, "%d sessions due in %d.%06d\n", n, (int)a.due.tv_sec,  (int)a.due.tv_usec);
	n = select(a.maxfd + 1, &r, &w, NULL, &a.due);
	gettimeofday(&a.now, NULL);
	if (n <= 0) {
	    FD_ZERO(&r);
	    FD_ZERO(&w);
	    DBG(2, "select returns %d\n", n);
	    /* still call handlers on timeouts and signals */
	}
	for (n = 0; wait3(NULL, WNOHANG, NULL) >0; n++) ;
	if (n)
		DBG(1, "%d children terminated\n", n);
	a.run = 1; /* now execute the handlers */
	for (ps = &me->sess; (s = *ps) ;) {
	    DBG(2, "handle session %p\n", s);
	    me->cur = s;
	    me->app = s->app;
	    nexts = s->next;
	    if (s->cb(s, &a))	/* socket dead, unlink */
		*ps = nexts;
	    else
		ps = &s->next;
		
	}
    }
    return 0;
}

int main(int argc, char *argv[])
{
    struct app **app, *a;

    memset(&__me, 0, sizeof(__me));
    __me.all_apps = all_apps;
    /* main program arguments */
#ifndef NODEBUG
    int i;
    for (i = 1 ; i < argc; i++) {
        char *optval, *opt = argv[i];
        /* options without arguments */
        if (!strcmp(opt, "-v") || !strcmp(opt, "--verbose")) {
            verbose ++;
            __me.verbose ++;
            continue;
        }
        if (argc < 3)
            break;
        /* options with argument */
        optval = argv[i+1];
        break;
    }
#endif
    for (app = all_apps; (a = *app); app++) {
        __me.app = a;
	if ( a->init)
	    a->init();
	if ( a->parse)
	    a->parse(&argc, argv);
	if ( a->start)
	    a->start();
    }
    mainloop(&__me);
    return 0;
}
