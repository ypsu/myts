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
 * $Id: myts.h 7958 2010-12-04 01:15:04Z luigi $

This is a framework for event-based programming.
The entire state of the application is reachable through
a global "struct my_args _me".

Each sub-application is described by a "struct app" which
contains callbacks for boot, argument parsing, callbacks,
destructor, and private data.

 */

#ifndef _MYTS_H_
#define	_MYTS_H_
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>	/* gettimeofday */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>	/* inet_aton */

extern int verbose;
#ifdef NODEBUG
#define DBG(...)
#else
#define DBG(level, format, ...)  do {                   \
        if (verbose >= level) {   \
		struct timeval now; gettimeofday(&now, NULL); \
                fprintf(stderr, "%5d.%03d [%-14.14s %4d] " format,       \
			(int)(now.tv_sec %86400), (int)(now.tv_usec / 1000), \
                        __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        } } while(0)
#endif

/*
 * descriptor of an application.
 */
struct app {
	int (*init)(void);
	int (*parse)(int *argc, char *argv[]);
	int (*start)(void);
	int (*end)(void);
	void *data;	/* pointer to private data */
};

/*
 * callback in 'prepare' mode returns 1 if fd active.
 * callback in 'run' mode returns 0 if ok, 1 if dying.
 *	destruction must be done in the callback itself
 */
struct cb_args {
	struct timeval now;
	struct timeval due;	/* earliest due descriptor */
	fd_set *r;
	fd_set *w;
	int maxfd;
	int run;	/* 0: prepare select, 1: run */
};

typedef int (*cb_fn)(void *sess, struct cb_args *a);
struct sess {
	struct sess *next;
	struct app *app;	/* parent application */
	cb_fn	cb;
	void *arg;	/* identifier */
	int fd;
};

/*
 * All sessions should start with a 'struct sess'
 */

/*
 * my_args contains all the arguments for the program
 * The current app is me->app
 * The current session is me->sess;
 */
struct my_args {
	struct app **all_apps;	// array of all applications
	struct app *app;	// app under service
	struct sess *sess;
	struct sess *tmp_sess;
	struct sess *cur;	// session under service
	int verbose;	/* allow read all file systems */
};
extern struct my_args __me;

/*
 * Constructor for a new session
 */
void *new_sess(int size, int fd, cb_fn cb, void *arg);

/* add a millisecond value to a timer */
void timeradd_ms(const struct timeval *src, int ms, struct timeval *dst);
/* set dst to the min of the two */
void timersetmin(struct timeval *dst, const struct timeval *cur);
/* returns true if dst is set and <= 'now' */
int timerdue(const struct timeval *dst, const struct timeval *now);

extern int bytesperchar;

#endif /* _MYTS_H_ */
