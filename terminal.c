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
 * $Id: terminal.c 8169 2011-01-07 17:32:36Z luigi $
 *
 * terminal routines. This code interfaces with a shell session
 * over a PTY, and interprets ANSI control sequences to render the
 * screen into a text buffer, which can then be exported to clients
 * for display.
 */

#include "myts.h"
#include "terminal.h"

#include <signal.h>	/* kill */
#include <termios.h>	/* struct winsize */
#ifdef linux
#include <pty.h>
#else
#include <libutil.h>  /* forkpty */
#endif
#include <errno.h>
#include <ctype.h>      /* isalnum */

#define KMAX	256	/* keyboard queue */
#define SMAX	256	/* screen queue */

#define BYTES bytesperchar

#define UTF8 (BYTES==2)

/*
 * flags for terminal emulation.
 * kf_priv	cursor keys mode
 * kf_nocursor is set to hide the cursor ESC [?25l
 * kf_graphics means we received the ESC-(B command
 * kf_dographic means we are in "graphic" mode, where we
 * enter by either ESC-(B or SO, and exit with ESC-(0 or SI
 * kf_wrapped is used to manage wrapping -- when we write to
 * the last char of a line, do not advance the cursor but set the
 * marker, which is then used to handle future scroll sequences
 */
enum {
	kf_priv = 1,
	kf_nocursor =2,
	kf_graphics = 4, kf_dographic = 8,
	kf_insert = 0x10,
	kf_autowrap = 0x20,
	kf_wrapped = 0x40,
};
/*
 * values used for the 'attribute' page. The low 3 bits are used
 * for foreground color, the next 3 bits are background color.
 */
enum {
	ka_fg_shift = 0,	/* foreground mask shift */
	ka_bg_shift = 3,	/* backround mask shift */
	ka_fg= 0x07,	/* foreground mask */
	ka_bg = 0x38,	/* background mask */
};

/*
 * struct my_sess describes a shell session to which we talk.
 */
struct my_sess {
	struct sess sess;
	char *name;     /* session name */
	int pid;        /* pid of the child */
	void (*cb)(struct sess *);

	/* screen/keyboard buf have len *pos. *pos is the next byte to send */
	int kseq;       // need a sequence number for kb input ?
	int klen;       /* pending input for keyboard */
	char keys[KMAX];
	int kflags;     /* dec mode etc */
	int slen;       /* pending input for screen */
	char sbuf[SMAX];

	/* store pagelen instead of recomputing it all the times */
	int rows, cols, pagelen; /* geometry */
    int allrows, toprow, top;
	int cur;        /* cursor offset */
	int modified;   /* ... since last read */
	int nowrap;     /* do not wrap lines */
    int originmode;
	/* the scroll region (in rows, defaults to 0..rows-1).
	 * we store row, i.e. the first line to be left unchanged.
	 */
	int scroll_top, scroll_bottom;
	/* the page is made of rows*cols chars followed by attributes
	 * with the same layout
	 */
	/*
	 * attributes -- we use bits for foreground and bg color.
	 */
	uint8_t		cur_attr;	/* current attributes */

    int sb_lines;
    char *sb_page;
    char *sb_attributes;
    unsigned short *page16;
    char *attributes;
	char *page;     /* dump of the screen */
};

int term_keyin(struct sess *sess, char *k)
{
	struct my_sess *sh = (struct my_sess *)sess;

        /* map arrow keys to DEC in private mode. */
        if ((sh->kflags & kf_priv) && strlen(k) > 2 &&
			k[0] == '\033' && k[1] == '[' && index("ABCD", k[2])) {
		    k[1] = 'O';
        }

        /* silently drop chars in case of overflow */
        strncat(sh->keys + sh->klen, k, sizeof(sh->keys) - 1 - sh->klen);
        sh->klen = strlen(sh->keys);
	return 0;
}


/* return the 'modified' flag.
 * If ptr is set, clears the modified flag and returns
 * the info to access the screen area.
 */
int term_state(struct sess *sess, struct term_state *ptr)
{
	struct my_sess *sh = (struct my_sess *)sess;
	int ret;
	if (!sh)
		return 0;
	ret = sh->modified;
	DBG(2, "called on %s %s modified %d\n", sh->name,
		ptr ? "reset" : "keep", ret);
	if (ptr) {
		if (ptr->flags & TS_MOD)
			sh->modified = ptr->modified;
		else
			ptr->modified = sh->modified;
		if (ptr->flags & TS_CB)
			sh->cb = ptr->cb;
		else
			ptr->cb = sh->cb;
		if (ptr->flags & TS_NAME)
			sh->name = ptr->name;
		else
			ptr->name = sh->name;
		ptr->rows = sh->rows;
		ptr->cols = sh->cols;
		ptr->cur = (sh->kflags & kf_nocursor) ? -1 : sh->cur;
		ptr->data = sh->page;
		ptr->attr = sh->attributes;
		ptr->sb_data = sh->sb_page;
		ptr->sb_attr = sh->sb_attributes;
        ptr->top = sh->top;
	}
	return ret;
}

/* erase part of the 'screen' from 'start' for 'len' chars.
 * also taking care of the attributes.
 */
static void erase(struct my_sess *sh, int start, int len)
{
	char *x = sh->page + start*BYTES;
	DBG(3, "x %p start %d pagelen %d len %d\n", x, start, sh->pagelen, len);
    if(UTF8) {
        uint16_t *p=(uint16_t *)x;
        int i;
        for(i=0;i<len;i++)*p++=0x20;
    } else memset(x, ' ', len);
	memset(sh->attributes+start, sh->cur_attr, len);
}

/* scroll up one line, erase last line */
static void page_scroll(struct my_sess *sh)
{
	char *p = sh->page + sh->scroll_top * sh->cols * BYTES;
	int l = (sh->scroll_bottom - sh->scroll_top - 1) * sh->cols;
DBG(1, " scroll %i %i  %i  %i\n", sh->scroll_top, sh->scroll_bottom, l, p-sh->page);

    if(!sh->scroll_top && sh->sb_lines) {
        char *t;
        if (sh->top<sh->sb_lines-1)sh->top++;
        if(sh->top>1) {
            t=sh->sb_page+(sh->sb_lines-sh->top)*sh->cols*BYTES;
            memmove(t-sh->cols*BYTES, t, (sh->top)*sh->cols*BYTES);
            t=sh->sb_attributes+(sh->sb_lines-sh->top)*sh->cols;
            memmove(t-sh->cols, t, (sh->top)*sh->cols);
        }
        memcpy(sh->sb_page+(sh->sb_lines-1)*sh->cols*BYTES, sh->page, sh->cols*BYTES);
        memcpy(sh->sb_attributes+(sh->sb_lines-1)*sh->cols, sh->attributes, sh->cols);

    }
	memmove(p, p + sh->cols*BYTES, l*BYTES);
	p = sh->attributes + sh->scroll_top * sh->cols;	/* move to attributes */
	memmove(p, p + sh->cols, l);
	erase(sh, (sh->scroll_bottom - 1)*sh->cols, sh->cols);
}

static void page_scrolldown(struct my_sess *sh)
{
	char *p = sh->page + sh->scroll_top * sh->cols * BYTES;
	int l = (sh->scroll_bottom - sh->scroll_top - 1) * sh->cols;
DBG(0, " scrolldown %i %i  %i  %i\n", sh->scroll_top, sh->scroll_bottom, l, p-sh->page);
	memmove(p+ sh->cols*BYTES, p, l*BYTES);
	p = sh->attributes + sh->scroll_top * sh->cols;	/* move to attributes */
	memmove(p+ sh->cols, p, l);
	erase(sh, sh->scroll_top * sh->cols, sh->cols);
}
#define B() do {	\
        if(sh->originmode) { \
		if (sh->cur < sh->scroll_top*sh->cols) {	\
			DBG(0, "cur %d\n", sh->cur); \
			sh->cur = sh->scroll_top*sh->cols;	\
		} else if (sh->cur > sh->scroll_bottom*sh->cols) { \
			DBG(0, "cur %d\n", sh->cur); \
			sh->cur = sh->scroll_bottom*sh->cols;	\
		} \
        } else { \
		if (sh->cur < 0) {	\
			DBG(0, "cur %d\n", sh->cur); \
			sh->cur = 0;	\
		} else if (sh->cur > sh->pagelen) { \
			DBG(0, "cur %d\n", sh->cur); \
			sh->cur = sh->pagelen;	\
		} \
        } \
	} while(0)
/*
 * interpret a CSI sequence. Return 0 if all ok, 1 if the sequence
 * is incomplete so we should wait for more chars. In all cases, *s
 * points to the first unused character.
 * Codes are taken from the FreeBSD 'syscons' driver.
 */
static int do_csi(struct my_sess *sh, char **s, int curcol)
{
	/* see http://en.wikipedia.org/wiki/ANSI_escape_code */
	char *x, *parm, *base = *s + 2, cmd, mark=' ';
	(void) x;
	int n;
	int a1= 1, a2= 1, a3 = 1;

	DBG(3, "+++ CSI FOUND col=%i %i  ESC-%c%c%c%c%c\n", sh->cur/sh->cols+1, sh->cur%sh->cols, (*s)[1], (*s)[2],(*s)[3],(*s)[4],(*s)[5]);
	/* index() matches a NUL, so we need to check before */
	if (!*base)
		return 1;	// process later
	if (index("<=>?", *base)) /* private ANSI code */
		mark = *base++;
	if (!*base)
		return 1; // process later
	// skip parameters
	for (parm = base; *parm && index("0123456789;", *parm); parm++) ;
	DBG(3, "+++ now PARM %s\n", parm);
	cmd = parm[0];
	if (!cmd)
		return 1; // process later
	*s = parm;
	/* XXX parse a variable number of args */
	n = sscanf(base, "%d;%d;%d", &a1, &a2, &a3);
	/* print potentially invalid commands */
	if (!index("ABCDGHJKPXdghlmrt", cmd))
	    DBG(0, "ANSI sequence (%d)(%d) %d %d %d cmd %d( ESC-[%.*s)\n",
		n, mark, a1, a2, a3, cmd, (parm+1 - base), base);	
	switch (cmd) {
	case 'A': // up, hang at curcol
        if(a1==0)a1=1;
		sh->cur -= sh->cols * a1;
		if (sh->cur < 0)
			sh->cur = curcol;
//        sh->kflags &= ~kf_wrapped;
		break;
	case 'B': // down, hang at curcol
        if(a1==0)a1=1;
		sh->cur += sh->cols * a1;
		if (sh->cur >= sh->pagelen)
			sh->cur = sh->pagelen -sh->cols + curcol;
//        sh->kflags &= ~kf_wrapped;
		break;
	case 'C':	// right
        if(a1==0)a1=1;
		if (a1 >= sh->cols - curcol)
			a1 = sh->cols - curcol - 1;
		sh->cur += a1;
		B();
//        sh->kflags &= ~kf_wrapped;
		break;
	case 'D': // left
        if(a1==0)a1=1;
		if (a1 > curcol)
			a1 = curcol;
		sh->cur -= a1;
		B();
//        sh->kflags &= ~kf_wrapped;
		break;
	case 'd':	/* vertical position absolute */
		if (a1 >= sh->rows)
			a1 = sh->rows;
		if (a1 < sh->scroll_top || a1 >= sh->scroll_bottom) {
			sh->scroll_top = 0;
			sh->scroll_bottom = sh->rows;
		}
		sh->cur = (a1 - 1)*sh->cols + curcol;
		B();
//        sh->kflags &= ~kf_wrapped;
		break;
	case 'G':	/* horizontal position absolute */
	case '`':	/* horizontal position absolute */
		if (a1 > sh->cols)
			a1 = sh->cols;
		sh->cur += (a1 -1 ) - curcol;
		B();
//        sh->kflags &= ~kf_wrapped;
		break;
	case 'g':	/* tab clear, ignore */
		break;
	case 'H':
	case 'f': // both are cursor position, ok
		DBG(2, "a1 %d a2 %d\n", a1, a2);
		if (a1 > sh->rows)
			a1 = sh->rows;
		else if (a1 < 1)
			a1 = 1;
        if(sh->originmode) a1+=sh->scroll_top;
		if (a2 > sh->cols)
			a2 = sh->cols;
		else if (a2 < 1)
			a2 = 1;
		// XXX a1 -1 or just a1 ?
		sh->cur = (a1 - 1)*sh->cols + a2 - 1;
		B();
        sh->kflags &= ~kf_wrapped;
		break;
	case 'h':	/* set mode/set dec mode, incomplete */
		if (mark == '?') {
		    switch (a1) {
		    case 1: /* Cursor keys mode. */
                sh->kflags |= kf_priv; /* set cursor mode */
                break;
		    case 3: /* 132 column mode. */
                sh->cur=0;
                curcol=0;
//                sh->kflags &= ~kf_wrapped;
                erase(sh, 0, sh->pagelen);
			    break;
		    case 25: /* Display cursor. */
                sh->kflags &= ~kf_nocursor;
                break;
		    case 6: /* Origin mode. */
                sh->originmode = 1;
                sh->cur = sh->scroll_top*sh->cols;
                sh->kflags &= ~kf_wrapped;
                break;
		    case 7: /* Autowrap mode. */
			    sh->nowrap = 0; // XXX
			    break;
		    case 2: /* DECANM: ANSI/VT52 mode. */
		    case 5: /* Inverse video. */
#if 0
			    t->t_stateflags |= TS_ORIGIN;
			    t->t_originreg = t->t_scrollreg;
			    t->t_cursor.tp_row = t->t_scrollreg.ts_begin;
			    t->t_cursor.tp_col = 0;
			    t->t_stateflags &= ~TS_WRAPPED;
			    teken_funcs_cursor(t);
#endif
		    case 8: /* Autorepeat mode. */
		    case 40: /* Allow 132 columns. */
		    case 45: /* Enable reverse wraparound. */
		    case 47: /* Switch to alternate buffer. */
		    default:
			goto notfound;
		    }
		} else {
		    switch (a1) {
		    case 4: 	/* insert mode */
		    default:
			goto notfound;
		    }
		}
		break;
	case 'J': /* erase display, fixed */
		if (n == 0)
			a1 = 0;
		if (a1 == 1) {	/* erase from top to cursor (including cursor) */
			erase(sh, 0, sh->cur+1);
		} else if (a1 == 2) { /* erase entire page */
			erase(sh, 0, sh->pagelen);
			// sh->cur = 0; // XXX msdos ansy.sys
		} else { /* erase from cursor to bottom */
			erase(sh, sh->cur, sh->pagelen - sh->cur);
		}
		break;
	case 'K': /* erase line, ok */
		if (n == 0)
			a1 = 0;
		if (a1 == 1) { /* from beg. to cursor (including cursor) */
			erase(sh, sh->cur - curcol, curcol+1);
		} else if (a1 == 2) { /* entire line */
			x = sh->page + sh->cur - curcol;
			erase(sh, sh->cur - curcol, sh->cols);
		} else { /* from cursor to end of line */
			x = sh->page + sh->cur;
			erase(sh, sh->cur, sh->cols - curcol);
		}
		break;
	case 'l': /* reset mode */
		/* n = 1, mark = '?' */
		if (mark == '?') { /* reset dec mode */
		    switch (a1) {
		    case 1: /* normal cursors */
                sh->kflags &= ~kf_priv; /* back to normal */
                break;
		    case 3: /* 132 column mode. */
                sh->cur=0;
                curcol=0;
//                sh->kflags &= ~kf_wrapped;
                erase(sh, 0, sh->pagelen);
			    break;
		    case 25: /* Hide cursor. */
                sh->kflags |= kf_nocursor;
                break;
		    case 6: /* Origin mode. */
                sh->originmode = 0;
                sh->cur = 0;
                sh->kflags &= ~kf_wrapped;
                break;
		    case 7: /* Autowrap mode. */
//			    sh->nowrap = 1; 
			    break;
		    case 2: /* DECANM: ANSI/VT52 mode. */
		    case 5: /* Inverse video. */
		    case 8: /* Autorepeat mode. */
		    case 12: // XXX what ?
		    case 40: /* Disallow 132 columns. */
		    case 45: /* Disable reverse wraparound. */
		    case 47: /* Switch to alternate buffer. */
		    default:
			// we got 12, 1000, 1049
			goto notfound;
		    }
		} else {	/* reset mode */
		    switch (a1) {
		    case 4: /* disable insert mode */
		    default:
			goto notfound;
		    }
		}
		/* XXX resets the INSERT flag */
		break;
    case 'L': 
        if((sh->cur>=sh->scroll_top*sh->cols)&&(sh->cur<sh->scroll_bottom*sh->cols)) {
            int save_top;
            save_top=sh->scroll_top;
            sh->scroll_top=sh->cur/sh->cols;
            while((a1>0) & (sh->scroll_top+1<sh->scroll_bottom)) {
                page_scrolldown(sh);
                a1--;
                sh->scroll_top++;
            }
            sh->scroll_top=save_top;
        }
        break;
	case 'm': /* set_graphic_rendition */
	    {
		/* right now ignore attributes, fix later */
		int i, arg[3] = {a1, a2, a3};
		if (n == 0) {
			arg[0] = 0;
			n = 1;
		}
		for (i=0; i < 3 && i < n; i++) {
		    switch(arg[i]) {
                case 0: /* reset */
                    sh->cur_attr = 0;
                    break;
                case 1: /* bold */
                case 4: /* underline */
                case 5: /* blink */
                case 7: /* reverse */
                case 22: /* remove bold */
                case 24: /* remove underline */
                case 25: /* remove blink */
                case 27: /* remove reverse */
                case 30: /* Set foreground color: black */
                case 31: /* Set foreground color: red */
                case 32: /* Set foreground color: green */
                case 33: /* Set foreground color: brown */
                case 34: /* Set foreground color: blue */
                case 35: /* Set foreground color: magenta */
                case 36: /* Set foreground color: cyan */
                case 37: /* Set foreground color: white */
                    DBG(2, "setattr fg %d\n", arg[i]);
                    sh->cur_attr &= ~ka_fg;
                    sh->cur_attr |= (37 - arg[i]);
                    break;
                case 39: /* Set default foreground color. */
                    DBG(2, "setattr fg %d\n", arg[i]);
                    sh->cur_attr &= ~ka_fg;
                    break;
                case 40: /* Set background color: black */
                case 41: /* Set background color: red */
                case 42: /* Set background color: green */
                case 43: /* Set background color: brown */
                case 44: /* Set background color: blue */
                case 45: /* Set background color: magenta */
                case 46: /* Set background color: cyan */
                case 47: /* Set background color: white */
                    DBG(1, "setattr bg %d\n", arg[i]);
                    sh->cur_attr &= ~ka_bg;
                    sh->cur_attr |= (47 - arg[i]) << ka_bg_shift;
                    break;
                case 49: /* Set default background color. */
                    DBG(1, "setattr bg %d\n", arg[i]);
                    sh->cur_attr &= ~ka_bg;
                    break;
                default:
                    goto notfound;
		    }
		}
	    }
		break;
    case 'M': 
        if((sh->cur>=sh->scroll_top*sh->cols)&&(sh->cur<sh->scroll_bottom*sh->cols)) {
            int save_top;
            save_top=sh->scroll_top;
            sh->scroll_top=sh->cur/sh->cols;
            if(sh->scroll_top+1<sh->scroll_bottom)while((a1>0)) {
                page_scroll(sh);
                a1--;
            }
            sh->scroll_top=save_top;
        }
        break;
	case 'P': /* delete n characters */
		if (curcol + a1 < sh->cols) {
			char *dst = sh->page + sh->cur*BYTES;
			int l = sh->cols - curcol - a1;
			memcpy(dst, dst + a1*BYTES, l*BYTES);
			dst = sh->attributes + sh->cur;
			memcpy(dst, dst + a1, l); /* attributes */
			erase(sh, sh->cur + l, a1);
		} else {
			erase(sh, sh->cur, sh->cols - curcol);
		}
		break;
	case 'r': /* change scroll region */
		DBG(2, "scroll region to %d, %d\n", a1-1, a2-1);
		/* change y scroll region to a1-1,a2-1,
		 * position cursor to row a1-1
		 */
		if (n == 0) {	/* defaults */
			a1 = 1;
			a2 = sh->rows;
		}
		if (a1 >= 1 && a1 < a2 && a2 <= sh->rows) {
			sh->scroll_top = a1 - 1;
			sh->scroll_bottom = a2;
			sh->cur = (a1 - 1) * sh->cols;
			B();
		}
		break;
    case 't': /* Window manipulation */
            if(n>0) {
                switch(a1) {
                    case 8: /* change text size */
                        break;
                }
            }
        break;
	case 'X':	/* erase char, the next n characters */
		if (a1 + curcol > sh->cols)
			a1 = sh->cols - curcol;
		erase(sh, sh->cur,  a1);
		break;
	default:
	notfound:
		DBG(0, "-- at %4d ANSI sequence (%d) %d %d %d ( ESC-[%c%.*s)\n",
			sh->cur,
			n, a1, a2, a3, mark, (parm+1 - base), base);	
	}
	return 0;
}


int utf8_to_ucs2 (const unsigned char * input, const unsigned char ** end_ptr)
{
    *end_ptr = input;
    if (input[0] == 0)
        return -1;
    if (input[0] < 0x80) {
        *end_ptr = input + 1;
        return input[0];
    }
    if ((input[0] & 0xE0) == 0xE0) {
        if (input[1] == 0 || input[2] == 0)
            return -1;
        *end_ptr = input + 3;
        return (input[0] & 0x0F)<<12 | (input[1] & 0x3F)<<6 | (input[2] & 0x3F);
    }
    if ((input[0] & 0xC0) == 0xC0) {
        if (input[1] == 0)
            return -1;
        *end_ptr = input + 2;
        DBG(4,"Found unicode char %02x %02x = %04x\n", input[0], input[1], (input[0] & 0x1F)<<6 | (input[1] & 0x3F));
        return
            (input[0] & 0x1F)<<6 | (input[1] & 0x3F);
    }
    return -1;
}

/*
 * append a string to a page, interpreting ANSI sequences
 * Returns a pointer to leftover chars.
 */
static char *page_append(struct my_sess *sh, char *s)
{
    const uint8_t special[] = {	/* box drawing chars, UTF8 and CP-437 */
	'?',	0xb1,	'?',	'?',	'?',	'?',	0xf8,	0xf1,
	'?',	'?',	0xd9,	0xbf,	0xda,	0xc0,	0xc5,	'?',
	'?',	0xc4,	'?',	'?',	0xc3,	0xb4,	0xc1,	0xc2,
	0xb3,	0xf3,	0xf2,	0xe3,	'?',	0x9c,	0xfa,	'?'
	};
     const uint16_t special16[] = {	/* box drawing chars, UTF8 and CP-437 */
    	0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
    	0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
    	0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
    	0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7, 0x0020
	};

    for (; *s; ) {
        int c;
        char *ns;
        int curcol;
       
        if(UTF8) {
            c=utf8_to_ucs2((const unsigned char *)s, (const unsigned char **)&ns);
            if(c<0){
                c=0xfffd;
                ns++;
            }
        } else {
           c = *s;
        }
        curcol = sh->cur % sh->cols;
        switch (c) {
            case '\r': /* CR */
                sh->cur -= curcol;
                if(sh->kflags&kf_wrapped)
                    sh->cur -= sh->cols;
                B();
                sh->kflags &= ~kf_wrapped; /* ??? */
                break;
            case 0x0e: /* shift-out */
                sh->kflags |= kf_dographic;
                break;
            case 0x0f: /* shift-in */
                sh->kflags &= ~kf_dographic;
                break;
            case 7:	/* BEL, ignore */
                break;
            case '\t':	/* XXX simplified version, 8-pos tabs */
                if (curcol >= sh->cols - 8)
                    sh->cur += (sh->cols - 1 - curcol);
                else
                    sh->cur += 8 - (curcol % 8);
//                sh->kflags &= ~kf_wrapped;
                B();
                break;
            case '\b': // backspace
                if (curcol > 0)
                    sh->cur--;
//                sh->kflags &= ~kf_wrapped;
                B();
                break;
            case '\033': /* escape */
                if (!s[1])
                    goto done;	// incomplete sequence, process later
                if (s[1] == '[' ) { // CSI found
                    if (do_csi(sh, &s, curcol))
                        goto done;	/* continue later */
                    ns=s+1;
                } else {
                    if (!index("()>=HcDEM#", s[1]))
                        DBG(0, "other ESC-%.*s\n", 1, s+1);
                    /*
                     * ESC-( 	charset G0 used
                     * ESC-) 	charset G1 used
                     * ESC-=	keypad mode 1
                     * ESC->	keypad mode 0
                     * ESC-H	memorize tab position as X
                     */
                    if (index("()", s[1])) { /* treat g0 and g1 the same */
                        if (!s[2])
                        return s; // process later
                        s += 2;
                        if(UTF8)ns+=2;
                        switch (*s) {
                            case '0':	/* g0_scs_special graphics */
                                DBG(1, "enter graphics at %d\n", sh->cur);
                                sh->kflags |= (s[-1] == '(') ?
                                    (kf_graphics | kf_dographic) :
                                    kf_dographic;
                                break;
                            case 'B':	/* g0_scs_us_ascii */
                                DBG(1, "exit graphics at %d\n", sh->cur);
                                sh->kflags &= ~(kf_graphics | kf_dographic);
                                break;
                            default:
                                DBG(0, "unrecognised ESC ( %c\n", *s);
                        }
                    } else if (index("H=>", s[1])) { /* ignore these */
                        /* H horiz. tab set, ignore */
                        /* = keypad app mode */
                        /* > keypad numeric mode */
                        s++;
                        if(UTF8)ns++;
                    } else if(s[1]=='c') { /* reset */
                        sh->cur=0;
                        curcol=0;
                        sh->top = 0;
//                        sh->kflags &= ~kf_wrapped;
                        s++;
                        if(UTF8)ns++;
                    } else if(s[1]=='D') { /* IND - line down */
                        sh->cur += sh->cols;
//                        sh->kflags &= ~kf_wrapped;
                        B();
                        s++;
                        if(UTF8)ns++;
                    } else if(s[1]=='E') { /* NEL - New line */
                        sh->cur -=curcol;
                        if(sh->kflags&kf_wrapped) {
                            sh->kflags &= ~kf_wrapped;
                        } else {
                            DBG(1," \\n: cur=%i\n", sh->cur);
                            sh->cur += sh->cols;
                        }
                        while (sh->cur >= sh->scroll_bottom * sh->cols) {
                            sh->cur -= sh->cols;
                            B();
DBG(0, "auto Scrolling\n");
                            page_scroll(sh);
                        }
                        s++;
                        if(UTF8)ns++;
                    } else if(s[1]=='M') { /* RI - reverse index */
                        sh->cur -= sh->cols;
                        if(sh->cur < sh->scroll_top*sh->cols) {
                            sh->cur += sh->cols;
                            page_scrolldown(sh);
                        }
                        sh->kflags &= ~kf_wrapped;
                        B();
                        s++;
                        if(UTF8)ns++;
                    } else if(s[1]=='#') { /* DEC */
                        DBG(0, "ESC-# %c, ignoring.\n", s[2]);
                        if(s[2]=='8') {
                            if(UTF8) {
                                int i;
                                for(i=0;i<sh->pagelen;i++)sh->page16[i]='E';
                            } else memset(sh->page, 'E', sh->pagelen);
                            memset(sh->attributes, sh->cur_attr, sh->pagelen);
                        }
                        s+=2;
                        if(UTF8)ns+=2;
                    } else {
                        DBG(0, "non ANSI sequence %d ESC-%c\n", s[1], s[1]);
                        s++;	/* skip the char */
                        if(UTF8)ns++;
                    }
                }
                break;
            default:	/* all other chars */
                if (c == '\n') {
                    if(sh->kflags&kf_wrapped) {
                        sh->kflags &= ~kf_wrapped;
                    } else {
                        DBG(1," \\n: cur=%i\n", sh->cur);
                        sh->cur += sh->cols;
                    }
                }
                while (sh->cur >= sh->scroll_bottom * sh->cols) {
                    sh->cur -= sh->cols;
                    B();
DBG(0, "auto Scrolling\n");
                    page_scroll(sh);
                }
                if (c != '\n') { /* already handled above */
                    if (c >= 0x60 && c < 0x7f &&
                        (sh->kflags & kf_dographic) && sh->kflags & kf_graphics) { 
                        if(UTF8) 
                            sh->page16[sh->cur] = special16[(c - 0x60)];
                        else sh->page[sh->cur] = special[(c - 0x60)];
                    } else {
                        if(UTF8) {
                                sh->page16[sh->cur] = c;
                        } else {
                            sh->page[sh->cur] = c;
                        }
                        sh->attributes[sh->cur] = sh->cur_attr;
                        sh->kflags &= ~kf_wrapped;
                    }
                    if(curcol==sh->cols-1) {
                        sh->kflags |= kf_wrapped;
                    }
                    sh->cur++;
                    if (sh->cur > sh->pagelen) {
                        DBG(0,"--- ouch, overflow on c %d\n", c);
                        sh->cur = 0; // XXX what should we do ? */
                    }
                }
        }
        if(UTF8) {
            s=(char *)ns;
        } else {
            s++;
        }
    }
done:
    if (*s) {
        DBG(3, "----- leftover stuff ESC [%s]\n", s+1);
    }
    return s;
}

static int term_keyboard(struct my_sess *sh)
{
	int l = write(sh->sess.fd, sh->keys, sh->klen);
	if (l <= 0) {
		DBG(1, "error writing to keyboard\n");
		return 1; /* error, currently ignored */
	}
	if (l < sh->klen)
		DBG(0, "short write to keyboard %d out of %d\n", l, sh->klen);
	// ioctl(sh->sess.fd, TIOCDRAIN); // XXX blocks
	strcpy(sh->keys, sh->keys + l);
	sh->klen -= l;
	return 0;
}

/* process screen output from the shell */
static int term_screen(struct my_sess *sh)
{
	char *s;
	int spos = strlen(sh->sbuf);
	int l = read(sh->sess.fd, sh->sbuf + spos, sizeof(sh->sbuf) - 1 - spos);

	if (l <= 0) {
		DBG(0, "--- shell read error, dead %d\n", l);
		sh->sess.fd = -1; /* report error. */
		return 1;
	}
	spos += l;
	sh->sbuf[spos] = '\0';
	DBG(2, "got %d bytes for %s\n", l, sh->name);
	sh->modified = 1; /* maybe not... */
	s = page_append(sh, sh->sbuf); /* returns unprocessed pointer */
	strcpy(sh->sbuf, s);
	return 0;
}

/*
 * Callback for I/O with the shell
 */
int handle_shell(void *_s, struct cb_args *a)
{
	struct my_sess *sh = _s;

	if (sh->sess.fd < 0) { /* dead */
		if (a->run == 0)
			return 0;
		if (sh->cb)
			sh->cb(_s);
		free(sh);	/* otherwise destroy */
		return 1;
	}
	DBG(1, "poll %p %s\n", sh, sh->name);
	if (a->run == 0) {
		FD_SET(sh->sess.fd, a->r);
		if (sh->klen)	/* have bytes to send to keyboard */
			FD_SET(sh->sess.fd, a->w);
		return 1;
	}
	if (FD_ISSET(sh->sess.fd, a->w))
		term_keyboard(sh);
	if (FD_ISSET(sh->sess.fd, a->r))
		term_screen(sh); /* can close the fd. handle later */
	return 0;
}

/*
 * Create a "terminal" child process, and talk to it through a slave pty
 * using event-based sessions. "name" is the identifier.
 */
struct sess *term_new(char *cmd, const char *name,
	int rows, int cols, int sb_lines, void (*cb)(struct sess *))
{
        int l, ln = strlen(name) +1;
	struct winsize ws;
	struct my_sess *s;

	if (rows < 4 || rows > 80)
		rows = 25;
	if (cols < 10 || cols > 160)
		cols = 80;
	l = rows*cols;
    
	DBG(1, "create shell %s %s %dx%d\n", name, cmd, rows, cols);
	/* allocate space for page and attributes */
        if(UTF8) {
            if ((sizeof(*s)+ln)&1) ln++; /* make sure page is aligned */
        }
        s = new_sess(sizeof(*s) + ln + l*(1+BYTES) + cols*(1+BYTES)*sb_lines+1 , -2, handle_shell, NULL);
        if (!s) {
		DBG(0, "failed to create session for %s\n", name);
		return NULL;
	}
    s->cb = cb;
    s->allrows = 1000;
    s->rows = rows;
    s->cols = cols;
    s->pagelen = s->rows * s->cols;
    s->modified = 1;
    s->nowrap = 0;	
    s->originmode = 0;
    s->scroll_top =0;
    s->scroll_bottom = rows;
    s->cur = 0;
    s->cur_attr = 0;
    s->top=0;

    s->name = (char *)(s + 1);
    s->page = s->name + ln;
    s->page16 = (unsigned short *)s->page;
    s->attributes=s->page+l*BYTES;
    s->sb_lines = sb_lines;
    if(sb_lines) {
        s->sb_page=(char *)(((unsigned long)(s->page+l*(1+BYTES)+1))&(~1ul));
        s->sb_attributes=s->sb_page+sb_lines*cols*BYTES;
    } else s->sb_page=NULL;
    erase(s, 0, s->pagelen);
    strcpy(s->name, name);

    bzero(&ws, sizeof(ws));
	ws.ws_row = rows;
	ws.ws_col = cols;
	s->pid = forkpty(&s->sess.fd, NULL, NULL, &ws);
	DBG(3, "forkpty gives pid %d pty %d\n", s->pid, s->sess.fd);
	if (s->pid < 0) {
	    DBG(0, "forkpty failed\n");
	    /* failed. mark session as dying, will be freed later */
	    s->sess.fd = -1; /*mark as dying */
	    return NULL;
	}
	if (s->pid == 0) { /* this is the child, execvp the shell */
	    char *av[] = { cmd, NULL};
	    //putenv("TERM=linux");
        putenv("ENV=/mnt/us/myts/profile");
	    execvp(av[0], av);
	    exit(1); /* notreached normally */
	}
	fcntl(s->sess.fd, F_SETFL, O_NONBLOCK);
        return (struct sess *)s;
}

const char *term_name(struct sess *s)
{
	return s->cb == handle_shell ? ((struct my_sess *)s)->name : "";
}

int term_kill(struct sess *sess, int sig)
{
	struct my_sess *sh = (struct my_sess *)sess;
	if (sh)
		kill(sh->pid, sig);
	return 0;
}

struct sess *term_find(const char *name)
{
	struct sess *s;

	for (s = __me.sess; s; s = s->next) {
		if (s->cb == handle_shell && !strcmp(name, ((struct my_sess *)s)->name))
			break;
	}
	return s;
}
