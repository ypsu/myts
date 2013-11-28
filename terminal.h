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
 *
 * $Id: terminal.h 7959 2010-12-04 07:37:03Z luigi $
 */

#ifndef _TERMINAL_H_
#define _TERMINAL_H_

/*
 * terminal support for kiterm and launchpad.
 * The routines support creation of a terminal session,
 * various manipulations, sending characters to the terminal,
 * and exporting the framebuffer.
 */

/*
 * term_new creates a session, and possibly specifies a callback to invoke
 * on special events (typically destruction).
 */
struct sess *term_new(char *cmd, const char *name,
	int rows, int cols, int sb_lines, void (*cb)(struct sess *));

/* lookup a session by name */
struct sess *term_find(const char *name);

/* return the name */
const char *term_name(struct sess *s);

/* send nul-terminated string to the terminal */
int term_keyin(struct sess *, char *k);

/* send a signal to the terminal session */
int term_kill(struct sess *sh, int sig);

/*
 * terminal state. The flags can be used to update modified, callback, name
 * when calling term_state(s, ptr) with a non-null ptr.
 * For convenience, term_state() returns the 'modified' state.
 */
enum { TS_MOD = 1, TS_CB = 2, TS_NAME = 4 };
struct term_state {
	int flags;
	int modified, rows, cols, cur;
	int pid;
    int top;
	void (*cb)(struct sess *);
	char *name;
	char *data;
    char *attr;
	char *sb_data;
    char *sb_attr;
};
int term_state(struct sess *sh, struct term_state *ptr);


#endif /* _TERMINAL_H* */
