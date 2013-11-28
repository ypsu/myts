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
 * Inspired by launchpad code from Andy M. aka h1uke	h1ukeguy @ gmail.com
 * but there is barely any of the original code left here.
 */

/*
 * launchpad+terminal+webserver program for Kindle.
 * See top level README for details,
 * and config.c for the config file format.
 */

#define _GNU_SOURCE	/* asprintf */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>	/* dirname */

#include <sys/stat.h>
#include <signal.h>

#include <linux/input.h>

#include "myts.h"
#include "config.h"
#include "dynstring.h"
#include "terminal.h"
#include "pixop.h"
#include "font.h"
#include "screen.h"

int utf8_to_ucs2 (const unsigned char * input, const unsigned char ** end_ptr);

/*
 * Each key entry has a name, a type and one or two parameters.
 * The name is normally the ascii char (case sensitive)
 * or the key name (case insensitive).
 * The type is 0 for send, 1 for sendshift, 2 for sendfw, 3 for sym.
 * The code is the code to send, or the y steps for symbols.
 * Additionally, symbols need an x steps.
 */
enum k_type { KT_SEND = 0, KT_FW, KT_VOL, KT_SHIFT, KT_ALT, KT_SYM };
struct key_entry {
	char 	*name;	/* not null terminated */
	uint8_t namelen;
	uint8_t	type;	/* send, sendshift, sym */
	uint8_t	code;	/* or x-steps if sym */
	uint8_t ysteps;	/* if sym */
};

/* each I/O channel has different name and fd for input and output */
struct iodesc {
	char *namein;
	char *nameout;
	int fdin;
	int fdout;
};

/* support for multiple terminal sessions */
struct terminal {
	struct terminal *next;
	struct sess *the_shell;
	char name[0]; 	/* dynamically allocated */
};

/*
 * Overall state for the launchpad.
 * The destructor must:
 *	- preserve all terminal sessions
 *	- call cfg_free(db)
 *	- call cfg_read and init
 */
struct lp_state {
	/* e[] contains events sorted by name, with nentries entries.
	 * by_code[] is a direct access array to get an event by code,
	 * with pointers into e[]
	 */
	int		nentries;
	struct key_entry e[256];	/* table of in/out events */
	struct key_entry *by_code[256];	/* events by code */

	/* actions is a list of configured actions, pointing into the
	 * [actions] section of the db.
	 */
	struct config	*db;		/* the database */

	int		xsym, ysym;	/* initial SYMBOL position (1, 1) */
	/* codes for various keys */
	int		fw_left, fw_right, fw_up, fw_down, fw_select;
	int		del, sym;
	int		term_end, term_esc, term_shift, term_ctrl, term_sym;
	int		term_lang, term_fn, term_home;
    int     term_scrollup, term_scrolldown;


	int		refresh_delay;	/* screen refresh delay		*/
	struct iodesc	kpad, fw, vol, special;	/* names and descriptors	*/

    int fontheight, fontwidth;
    int xofs, yofs;

	/* fb, curterm, save_pixmap are either all set or all clear */
	struct terminal *curterm;	/* current session		*/
	fbscreen_t	*fb;		/* the framebuffer		*/
	dynstr		save_pixmap;	/* saved pixmap			*/

	/* various timeouts, nonzero if active */
	struct timeval	screen_due;	/* next screen refresh		*/

	volatile int	got_signal;	/* changed by the handler */

    int sb_lines, sb_pos, sb_step;

	/* This area must be preserved on reinit */
	int		savearea[0];	/* area below preserved on reinit */
	struct terminal *allterm;	/* all terminal sessions	*/
	char		basedir[1024];
	char		*cfg_name;		/* points into basedir */
	int		verbose;
};


static struct lp_state lpad_desc;
static struct lp_state *lps;

static char *symbols, *langsymbols, *shiftlangsymbols;

static unsigned char *langsymbols16[29], *shiftlangsymbols16[29];

const char *fnk[] = {
    "q\e[11~",  // F1
    "w\e[12~",  // F2
    "e\e[13~",  // F3
    "r\e[14~",  // F4
    "t\e[15~",  // F5
    "y\e[17~", // F6
    "u\e[18~", // F7
    "i\e[19~", // F8
    "o\e[20~", // F9
    "p\e[21~", // F10
    "a`",
    "s%",
    "d^",
    "f<",
    "g>",
    "h[",
    "j]",
    "k=",
    "l\e[23~", // F11
    "D\e[24~", // F12
    "z\t",
    "x;",
    "c,",
    "v(",
    "b)",
    "n{",
    "m}",
    ".,",
    NULL 
};

void process_screen(void);
void print_keymap();
static void capture_input(int capture);

static int is_kindle3(void)
{
	/* if all 3 /dev/input/event[012] are open then we are on Kindle3 */
	return (lps->kpad.fdin != -1 && lps->fw.fdin != -1 && lps->vol.fdin != -1);
}

/*
 * compare two key entries. Sorting is by length and then by string.
 * One-byte entries are case-sensitive, other are case-insensitive.
 * ecmp1() is used for the initial sorting (removing duplicates)
 * and breaks ties looking at the type (privileging short sequences)
 */
static int ecmp(const void *_l, const void *_r)
{
	const struct key_entry *l = _l, *r = _r;
	int d = l->namelen - r->namelen;
	
	if (d != 0)
		return d;
	return (l->namelen == 1) ? (int)l->name[0] - (int)r->name[0] :
		strncasecmp(l->name, r->name, l->namelen);
}

static int ecmp1(const void *_l, const void *_r)
{
	const struct key_entry *l = _l, *r = _r;
	int d = ecmp(l, r);
	return d ? d : l->type - r->type;
}

/*
 * maps a keydef to the correct key_entry, returns NULL if not found.
 */
static struct key_entry *lookup_key(const char *key, int len)
{
	struct key_entry l, *k;

	if (*key == ' ') {
		len = 0;
		key = "Space";
	}
	l.name = (char *)key;
	l.namelen = len ? len : strlen(key);
	k = bsearch(&l, lps->e, lps->nentries, sizeof(l), ecmp);
	if (!k)
		DBG(0, "entry '%.*s' not found\n", len, key);
	return k;
}

/*
 * Helper function that, given "N = a b c d ..." appends to lps.e[]
 * the key_entry sections for the keydefs on the right side.
 * lps.nentries points to the next free slot.
 */
static int build_seq(struct section *sec)
{
	const struct entry *k;
	struct key_entry *e = lps->e + lps->nentries;

	if (sec == NULL) {
		DBG(0, "section not found\n");
		return 1;
	}
	DBG(2, "exploring section %p\n", sec);
	for (k = cfg_find_entry(sec, NULL); k; k = k->next) {
		int l;
		char *s = k->key;
		DBG(2, "found %s = %s\n", k->key, k->value);
		e->type = KT_SEND;	/* normal */
		e->code = atoi(s);
		if ( index("sfv", *s) ) { /* shift fiveway volume */
			e->type = (*s == 's') ? KT_SHIFT : (*s == 'f' ? KT_FW : KT_VOL);
			e->code = atoi(s+1);
		} else if (!strncasecmp(s, "row", 3)) {
			e->type = KT_SYM; /* row */
			e->ysteps = atoi(s+3);
			e->code = 0;
		}
		s = k->value;
		while ( (l = strcspn(s, " \t")) || *s ) {
			if (l == 0) {
				s++;
				continue;
			}
			if (*s == '\\') {
				s++;
				l--;
			}
			e->name = s;
			e->namelen = l;
			e++;
			e[0] = e[-1];
			e->code++;	/* prepare next entry */
			s += l;
		}
	}
	lps->nentries = e - lps->e;
	DBG(1, "done %d entries\n", lps->nentries);
	return 0;
}

/* fetch a string, int or key name from the config file,
 * and store the value in dst. type can be
 *	's'	to store a string;
 *	'i'	to store an int;
 *	'k'	to store a keycode (lookup the value on the right);
 */
static int setVal(struct section *sec, const char *key, int type, void *dst)
{
	struct key_entry *e;
	const struct entry *k = cfg_find_entry(sec, key);

	if (!k)
		return 1;

	switch (type) {
	case 's':	/* string */
		*(char **)dst = k->value;
		break;
	case 'i':	/* int */
		*(int *)dst = strtol(k->value, NULL, 0) ;
		break;
	case 'k':	/* keycode */
		e = lookup_key(k->value, strlen(k->value));
		if (e == NULL || (e->type == KT_ALT || e->type == KT_SHIFT)) {
			DBG(0, "Warning: no code for %s %s\n", key, k->value) ;
			return 1;
		}
		*(int *)dst = e->code ;
		break;
	}
	return 0;
}

/* set dst with the code of the key called 'key' */
static int setKey(const char *key, int *dst)
{
	struct key_entry *e = lookup_key(key, strlen(key));
	if (e)
		*dst = e->code;
	return (e ? 0 : 1);
}

/*
 * reinitialize.
 */
static int launchpad_init(char *path)
{
	struct section *sec ;
	int i;
	struct key_entry *e;
    char *encoding, *font;

	memset(lps, 0, (char *)&lps->savearea - (char *)lps);
	/* load initial values */
	lps->refresh_delay = 100;
	lps->kpad.fdin = lps->fw.fdin = lps->vol.fdin = lps->special.fdin = -1;
	if (path == NULL)
		path = lps->cfg_name;

	lps->db = cfg_read(path, lps->basedir, NULL);
	if ( lps->db == NULL) {
		DBG(0, "%s -- not found or bad\n", path) ;
		return -1 ;
	}
	/* load file identifiers */
	sec = cfg_find_section(lps->db, "Settings");
	if (!sec) {
		DBG(0, "section Settings not found\n") ;
		return -1;
	}
	/* load system-independent values */
	setVal(sec, "RefreshDelay", 'i', &lps->refresh_delay);
	setVal(sec, "KpadIn", 's', &lps->kpad.namein);
	setVal(sec, "KpadOut", 's', &lps->kpad.nameout);
	setVal(sec, "FwIn", 's', &lps->fw.namein);
	setVal(sec, "FwOut", 's', &lps->fw.nameout);
	setVal(sec, "VolIn", 's', &lps->vol.namein);
	setVal(sec, "VolOut", 's', &lps->vol.nameout);
	setVal(sec, "SpecialIn", 's', &lps->special.namein);
	setVal(sec, "SpecialOut", 's', &lps->special.nameout);

    if(setVal(sec, "Symbols", 's', &symbols)) {
        symbols="!@#$%^&*()*+#-_()&!?~$|/\\\"':";
    }

    setVal(sec, "Font", 's', &font);
    setVal(sec, "Encoding", 's', &encoding);
    if(setVal(sec, "FontHeight", 'i', &lps->fontheight)) lps->fontheight=16;
    if(setVal(sec, "FontWidth", 'i', &lps->fontwidth)) lps->fontwidth=8;
    if(setVal(sec, "YOffset", 'i', &lps->yofs)) lps->yofs=40;
    if(setVal(sec, "ScrollbackLines", 'i', &lps->sb_lines)) lps->sb_lines=0;
    lps->sb_pos=0;
    lps->xofs = 0;
    

	/* try open files so we know on what system we are */
	i = O_RDONLY | O_NONBLOCK;
	lps->kpad.fdin	= open(lps->kpad.namein, i);
	lps->fw.fdin	= open(lps->fw.namein, i);
	lps->vol.fdin	= open(lps->vol.namein, i);
	lps->special.fdin	= open(lps->special.namein, i);
	DBG(2, "open %s %s %s gives %d %d %d\n",
		lps->kpad.namein, lps->fw.namein, lps->vol.namein,
		lps->kpad.fdin, lps->fw.fdin, lps->vol.fdin);

	if (lps->kpad.fdin == -1 && lps->fw.fdin == -1 && lps->vol.fdin) {
		DBG(0, "no input available, exiting...\n") ;
		return -1;
	}
	i = O_WRONLY | O_NONBLOCK;
	lps->kpad.fdout	= open(lps->kpad.nameout, i) ;
	lps->fw.fdout	= open(lps->fw.nameout, i);
	lps->vol.fdout	= open(lps->vol.nameout, i);
	/* ignore errors on output */

	/* load keymap entries (system-dependent) */
	build_seq(cfg_find_section(lps->db, "inkeys"));
	build_seq(cfg_find_section(lps->db,
		is_kindle3() ? "inkeys-k3" : "inkeys-dx"));
	DBG(2, "sort sequences\n");

	/* sort using the extended comparison function */
	qsort(lps->e, lps->nentries, sizeof(*e), ecmp1);
	/* now remove duplicates, keeping the shortest sequences */
	for (e = lps->e, i = 0; i < lps->nentries - 1; i++, e++) {
	    if (ecmp(e, e+1) == 0) {
		    DBG(1, "dup %3d for ty %d code %3d y %3d l %d %.*s\n",
			i, e->type, e->code, e->ysteps,
			e->namelen, e->namelen, e->name);
		    e[1].type = 255;
	    }
	}
	/* second pass, bubble up all duplicate elements */
	for (e = lps->e, i = 0; i < lps->nentries; i++, e++) {
		if (e->type == 255) {
			lps->nentries--;
			*e = lps->e[lps->nentries];
		}
	}
	/* sort again, this time using ecmp */
	qsort(lps->e, lps->nentries, sizeof(*e), ecmp);

	/* build the 'by_code' array, used in output */
	memset(lps->by_code, 0, sizeof(lps->by_code));
	DBG(2, "--- dump events by name ---\n");
	for (e = lps->e, i = 0; i < lps->nentries; i++, e++) {
		if (e->type == KT_SEND || e->type == KT_FW)
			lps->by_code[e->code] = e;
		DBG(2, "%3d ty %d code %3d y %3d l %d %.*s\n",
				i, e->type, e->code, e->ysteps,
				e->namelen, e->namelen, e->name);
	}
	DBG(2, "--- debugging -- dump events by code ---\n");
	for (i=0; i < 256; i++) {
		e = lps->by_code[i];
		if (!e) continue;
		DBG(2, "%3d ty %d code %3d y %3d l %d %.*s\n",
				i, e->type, e->code, e->ysteps,
				e->namelen, e->namelen, e->name);
	}

	/* load parameters that depend on the key mapping */
	setVal(sec, "TermEnd", 'k', &lps->term_end);
	setVal(sec, "TermEsc", 'k', &lps->term_esc);
	setVal(sec, "TermCtrl", 'k', &lps->term_ctrl);
	setVal(sec, "TermShift", 'k', &lps->term_shift);
	setVal(sec, "TermSym", 'k', &lps->term_sym);
	setVal(sec, "TermFn", 'k', &lps->term_fn);
	setVal(sec, "TermLang", 'k', &lps->term_lang);
	setVal(sec, "TermHome", 'k', &lps->term_home);
	setVal(sec, "TermScrollUp", 'k', &lps->term_scrollup);
	setVal(sec, "TermScrollDown", 'k', &lps->term_scrolldown);
	lps->xsym = 1;	/* position of initial symbol */
	lps->ysym = 1;

	setKey("Sym", &lps->sym);
	setKey("Left", &lps->fw_left);
	setKey("Right", &lps->fw_right);
	setKey("Up", &lps->fw_up);
	setKey("Down", &lps->fw_down);
	setKey("Select", &lps->fw_select);
	setKey("Del", &lps->del);
	setKey("Home", &lps->term_home);

    if (init_font(encoding, font, lps->fontheight, lps->fontwidth)) {
        if(init_font("CP437.table", "unifont.hex", lps->fontheight, lps->fontwidth)) {
            DBG(0, "No font found.\n") ;
            return -1 ;
        }
    }

    if(bytesperchar==1) {
        if(setVal(sec, "LangSymbols", 's', &langsymbols)) {
            langsymbols="qwertyuiopasdfghjklDzxcvbnm.";
        }
        if(setVal(sec, "ShiftLangSymbols", 's', &shiftlangsymbols)) {
            shiftlangsymbols=langsymbols;
        }
    } else {
        char *utf8syms;
        char *utf8shiftsyms;
        int i;
        if(setVal(sec, "LangSymbols16", 's', &utf8syms)) {
            utf8syms=".ץקראטוןםפשדגכעיחלךףזסבהנמצת";
        }
        if(setVal(sec, "ShiftLangSymbols16", 's', &utf8syms)) {
            utf8shiftsyms=utf8syms;
        }
        for(i=0;i<28;i++) {
            langsymbols16[i]=(unsigned char *)utf8syms;
            utf8_to_ucs2((const unsigned char *)utf8syms, (const unsigned char **)&utf8syms);
            shiftlangsymbols16[i]=(unsigned char *)utf8shiftsyms;
            utf8_to_ucs2((const unsigned char *)utf8shiftsyms, (const unsigned char **)&utf8shiftsyms);
        }
        langsymbols16[i]=(unsigned char *)utf8syms;
        shiftlangsymbols16[i]=(unsigned char *)utf8shiftsyms;
    }
	return 0 ;
}

struct terminal *shell_find(const char *name);

/* block or unblock input events to the kindle.
 * If we detect the initiator, we block events, and release them
 * after we are done. Setting capture mode also creates a timeout
 * upon which we will unblock.
 */
static void capture_input(int capture)
{
	capture = capture ? 1 : 0 ; /* normalize */
	if (lps->kpad.fdin != -1 && ioctl(lps->kpad.fdin, EVIOCGRAB, capture))
    		perror("Capture kbd input:");
	if (lps->fw.fdin != -1 && ioctl(lps->fw.fdin, EVIOCGRAB, capture))
    		perror("Capture fw input:");
	if (lps->vol.fdin != -1 && ioctl(lps->vol.fdin, EVIOCGRAB, capture))
    		perror("Capture k3_vol input:");
}

static void curterm_end(void)
{
	int l = ds_len(lps->save_pixmap);

	DBG(0, "exit from terminal mode\n");
	if (l && lps->fb) {
		pixmap_t *p = &lps->fb->pixmap;
		memcpy(p->surface, ds_data(lps->save_pixmap), l);
		ds_reset(lps->save_pixmap);
		fb_update_area(lps->fb, UMODE_PARTIAL, 0, 0, p->width, p->height, NULL);
	}
	fb_close(lps->fb);
	lps->curterm = NULL;
	lps->fb = NULL;
	capture_input(0);
}

/*
 * pass keys to the terminal code, conversion etc. will happen there
 */
static void process_term(struct input_event *ev, int mode)
{
	char k[16];
	struct key_entry *e = lps->by_code[ev->code];
	/* simulate ctrl, shift, sym keys */
	static int ctrl = 0;
	static int shift = 0;
	static int sym = 0;
	static int fn = 0;
	static int lang = 0;
	static int langlock = 0;
	static int home = 0;
	(void) home;
    static int help = 0;

	DBG(1, "process event %d %d e %p %.*s for terminal\n",
		ev->value, ev->code, e,
		e ? e->namelen : 3, e ? e->name : "---");
	if (e == NULL)	/* unknown event */
		return;
	memset(k, 0, sizeof(k));
	if (ev->value == 1 || ev->value == 2) { /* press */
#define E_IS(e, s) ((e)->namelen == strlen(s) && !strncasecmp((e)->name, s, (e)->namelen))
		if (ev->code == lps->term_end) {
            if(shift||fn||ctrl||sym) {
                if(!help) print_keymap();
                help=1;
            }  else help=0;
			return;
        }
		if (ev->code == lps->term_shift)
			shift = 1;
		else if (ev->code == lps->term_ctrl)
			ctrl = 1;
		else if (ev->code == lps->term_sym)
			sym = 1;
		else if (ev->code == lps->term_fn)
			fn = 1;
		else if (ev->code == lps->term_lang) {
            if(shift)
                langlock = !langlock;
            else lang = 1;
        } 
        else if (ev->code == lps->term_scrollup) {
			lps->sb_pos += lps->sb_step;
            process_screen();
        }
		else if (ev->code == lps->term_scrolldown) {
			lps->sb_pos -= lps->sb_step;
            if (lps->sb_pos<0) lps->sb_pos=0;
            process_screen();
        }
		else if (ev->code == lps->term_home) {
            strcpy(k, shift ? "\eOF" : "\eOH"); // END or HOME
            home = 1;
        } 
		else if (fn) {
			/* map chars into escape sequences */
			int i;
			char c = ' ';
			if (e->namelen == 1)
				c = e->name[0];
			else if (E_IS(e, "Del"))
				c = 'D';
			for (i = 0; fnk[i]; i++) {
				if (fnk[i][0] == c) {
					strcpy(k, fnk[i]+1);
                    if (k[0] == '\t' && shift)
                        strcpy(k, "\e[Z"); // backtab
					break;
				}
			}
			DBG(1, "function %s\n", k);
		} 
        else if (sym) {
			/* translate. The first row in the table contains
			 * the base characters, and the other
			 * two are the mappings with SYM and SYM-SHIFT
			 */
			const char *t[] = { "qwertyuiopasdfghjklDzxcvbnm."};
			char *p;
			if (e->namelen == 1)
				k[0] = e->name[0];
			else if (E_IS(e, "Del"))
				k[0] = 'D';
//			else if (E_IS(e, "Sym"))
//				k[0] = 'S';
//			else if (E_IS(e, "Enter"))
//				k[0] = 'E';
			p = index(t[0], k[0]);
			if ((p != NULL) && (p-t[0]<=27)) {
                k[0] = symbols[p - t[0]];
                if (k[0] == '\t' && shift)
                    strcpy(k, "\e[Z"); // backtab
            }
		} 
        else if (lang || langlock) {
			/* translate. The first row in the table contains
			 * the base characters, and the other
			 * two are the mappings with SYM and SYM-SHIFT
			 */
			const char *t[] = { "qwertyuiopasdfghjklDzxcvbnm."};
			char *p;
			if (e->namelen == 1)
				k[0] = e->name[0];
			else if (E_IS(e, "Del"))
				k[0] = 'D';
			p = index(t[0], k[0]);
			if ((p != NULL) && (p-t[0]<=27)) {
                if(bytesperchar==1) {
                    k[0]=langsymbols[p - t[0]];
fprintf(stderr,"%i\n", k[0]);
                } else {
                    memcpy(k, langsymbols16[p-t[0]], langsymbols16[p-t[0]+1]-langsymbols16[p-t[0]]);
                }
            }
		} 
        if(!k[0]) {
        if (e->namelen == 1) {
			k[0] = e->name[0];
			if (isalpha(k[0])) {
				if (shift) // shift overrides control
					k[0] += 'A' - 'a';
				else if (ctrl)
					k[0] += 1 - 'a';
			} else if (isdigit(k[0])) {
				if (shift) // shift overrides control
					k[0] = ")!@#$%^&*("[k[0] - '0'];
				else if (ctrl)
					k[0] += 1 - 'a';
			}
		} else if (E_IS(e, "Enter"))
			k[0] = 13;
        else if (ev->code == lps->term_esc)
			k[0] = 0x1b;	/* escape */
		else if (E_IS(e, "Space"))
			k[0] = ' ';
		else if (E_IS(e, "Del"))
			k[0] = 0x7f;
		else if (E_IS(e, "Up"))	/* PgUp if shift pressed */
			strcpy(k, shift ? "\e[5~" : "\e[A");
		else if (E_IS(e, "Down")) /* PgDown if shift pressed */
			strcpy(k, shift ? "\e[6~" : "\e[B");
		else if (E_IS(e, "Right"))
			strcpy(k, "\e[C");
		else if (E_IS(e, "Left"))
			strcpy(k, "\e[D");
        }
	} else if (ev->value == 0) { /* release */
		if (ev->code == lps->term_end) {
			if(help) process_screen() ; else curterm_end();
            help=0;
			return;
		}
		if (ev->code == lps->term_shift)
			shift = 0;
		else if (ev->code == lps->term_ctrl)
			ctrl = 0;
		else if (ev->code == lps->term_sym)
			sym = 0;
		else if (ev->code == lps->term_fn)
			fn = 0;
		else if (ev->code == lps->term_lang)
			lang = 0;
		else if (ev->code == lps->term_home)
			home = 0;
	}
    if(k[0] && lps->sb_pos) {
        lps->sb_pos=0;
        process_screen();
    } 
	if (lps->curterm)
		term_keyin(lps->curterm->the_shell, k);
}


/*
 * print a buffer at x, y. If attr, use attributes array
 * Wrap after 'cols'.
 */
static void print_buf(int x0, int y0, int cols, int cur,
	const uint8_t *buf, int len, const uint8_t *attr, int bg0)
{
        int i, x = x0, y = y0;
        uint16_t *buf16=(uint16_t *)buf;
        pixmap_t char_pixmap;

        for (i=0; i < len; i++) {
            int cc;
            unsigned char bg = attr ? attr[i] : (bg0 << 2);
            if(bytesperchar==1) 
                cc = buf[i];
            else cc = buf16[i];
            bg = (bg & 0x38) >> 2; /* background color */

            bg = bg | (bg << 4);
            if ( i == cur )
                bg |= 0x88;
            get_char_pixmap(lps->fb->font, cc, &char_pixmap) ;
            pix_blt(&lps->fb->pixmap, x, y,
                &char_pixmap, 0, 0, -1, -1, bg) ;
            x += char_pixmap.width;
            if ( (i+1) % cols == 0) {
                x = x0;
                y += char_pixmap.height;
            }
        }
        if(y==y0)y+=char_pixmap.height;
	fb_update_area(lps->fb, UMODE_PARTIAL, x0, y0,
		cols*(char_pixmap.width), y - y0, NULL) ;
	DBG(2, "end\n");
}

/*
 * update the screen. We know the state is 'modified' so we
 * don't need to read it, just notify it and fetch data.
 * XXX to optimize the code we could store a copy of the prev
 * window so we only update smaller regions of the screen.
 */
void process_screen(void)
{
	struct term_state st = { .flags = TS_MOD, .modified = 0};
	uint8_t *d, *a;
    int r;

    r=0;

	timerclear(&lps->screen_due);
	if (!lps->curterm || !lps->fb)
		return;
	term_state(lps->curterm->the_shell, &st);

DBG(1, "st.top = %i   sb_pos = %i\n", st.top, lps->sb_pos);
    if(lps->sb_pos>st.top)lps->sb_pos = st.top;

    if(lps->sb_pos) {
        if (lps->sb_pos>=st.rows) {
            r=st.rows;
        } else {
            r=lps->sb_pos;
        }
        print_buf(lps->xofs, lps->yofs, st.cols, -1, 
                    (uint8_t *)st.sb_data+(lps->sb_lines-lps->sb_pos)*st.cols*bytesperchar, r * st.cols,
		            (uint8_t *)st.sb_attr+(lps->sb_lines-lps->sb_pos)*st.cols, 0);
    } 
    if(r<st.rows){

        d = (unsigned char *)st.data;
        a = (unsigned char *)st.attr;

        print_buf(lps->xofs, lps->yofs + r*lps->fontheight, st.cols, st.cur, d, (st.rows-r) * st.cols,
            a, 0);
    }
}

void print_buf8(int x0, int y0, int cols, int cur,
            const uint8_t *buf, int len, const uint8_t *attr, int bg0) {
    uint16_t buf16[65];
    int i;
    if(bytesperchar==2) {
        for(i=0;i<len;i++) buf16[i]=*buf++;
        print_buf(x0, y0, cols, cur, (const uint8_t *)buf16, len, attr, bg0);
        DBG(0," y=%i   %04x %04x %04x %04x %04x\n", y0, buf16[0], buf16[1], buf16[2], buf16[3],buf16[6]);
    } else 
        print_buf(x0, y0, cols, cur, buf, len, attr, bg0);
}


void print_keymap() {
    unsigned char buf[65];
    int i, j;
    struct term_state st = { .flags = TS_MOD, .modified = 0};
	timerclear(&lps->screen_due);
	if (!lps->curterm || !lps->fb)
		return;
	term_state(lps->curterm->the_shell, &st);

    print_buf8(0, lps->yofs+lps->fontheight*1, st.cols, -1, (unsigned char *)"    q     w     e     r     t     y     u     i     o     p     ",64 , NULL, 0);
    print_buf8(0, lps->yofs+lps->fontheight*4, st.cols, -1, (unsigned char *)"    a     s     d     f     g     h     j     k     l     D     ",64 , NULL, 0);
    print_buf8(0, lps->yofs+lps->fontheight*7, st.cols, -1, (unsigned char *)"    z     x     c     v     b     n     m     .                 ",64 , NULL, 0);

    memset(buf,' ',64);
    for(j=0;j<12;j+=3) 
        print_buf8(0, lps->yofs+lps->fontheight*j, st.cols, -1, buf, 64 , NULL, 0);
    for(j=0;j<30;j+=10) {
        memset(buf,' ',64);
        for(i=0;i<(j==20?8:10);i++) { 
            buf[i*6+3] = symbols[j+i];
            if(fnk[j+i][1]=='\033') {
                buf[i*6+5]='F';
                buf[i*6+6]=fnk[j+i][4];
                if(buf[i*6+6]>'5')buf[i*6+6]--;
                if(fnk[j+i][3]=='2') {
                    buf[i*6+6]="9a bc"[fnk[j+i][4]-'0'];
                }
            } else buf[i*6+5]=fnk[j+i][1];
        }
        print_buf8(0, lps->yofs+lps->fontheight*(2+(j/10)*3), st.cols, -1, buf, 64 , NULL, 0);
    }
    DBG(0, "Help\n");
}


/*
 * Process an input event from the kindle. 'mode' is the source
 */
static void process_event(struct input_event *ev, int mode)
{
	static int npressed = 0 ;	/* keys currently pressed */
	(void) npressed;

    if (mode == -3) { /* Special event */
        char *buf = (char *)ev;
        if(buf[0]=='A') {
            buf[2]='\0';
			lps->fb = fb_open();	/* also mark terminal mode */
			struct terminal *t = shell_find(buf);
			DBG(0, "start %s got %p\n", buf, t);
			if (t == NULL) {
                fb_close(lps->fb);
				return;
            }
			if (lps->fb) {	/* if success, input is for us */
				pixmap_t *pix = &lps->fb->pixmap;
				int l = pix->width * pix->height * pix->bpp / 8;
				lps->curterm = t;
				ds_reset(lps->save_pixmap);
				ds_append(&lps->save_pixmap, pix->surface, l);
				capture_input(1) ;
				// set a timeout to popup the terminal
				gettimeofday(&lps->screen_due, NULL);
            }
        }

        return;
    }
	if (ev == NULL) { /* SYNC */
		npressed = 0;
		return;
	}

	if (ev->type != EV_KEY)
		return;
	DBG(2, "event ty %d val %d code %d npress %d\n",
		ev->type, ev->value, ev->code,
		npressed);
	/* ignore autorepeat events, ev->value == 2. */
	if (lps->fb) {
		process_term(ev, mode);
		return;
	}
}

static void hup_handler(int x)
{
	lps->got_signal = 1 ; /* reinit */
}

static void int_handler(int x)
{
	lps->got_signal = 2 ; /* exit */
}

static void fd_close(int *fd)
{
	if (*fd == -1)
		return;
	close(*fd);
	*fd = -1;
}

static void free_terminals(void)
{
	struct terminal *t;

	curterm_end();

	while ( (t = lps->allterm) ) {
		lps->allterm = t->next;
		term_kill(t->the_shell, 9);
		free(t);
	}
}

void term_dead(struct sess *s)
{
	struct terminal **t, *cur;
	for (t = &lps->allterm; (cur = *t); t = &(*t)->next) {
		if (cur->the_shell != s)
			continue;
		DBG(0, "terminal %s is dead\n", cur->name);
		*t = cur->next;
		if (lps->curterm == cur)
			curterm_end();
		memset(cur, 0, sizeof(*cur));
		free(cur);
		return;
	}
	DBG(0, "could not find dead terminal %p\n", s);
}

/*
 * find or create a shell with the given name. The name string
 * is copied in the descriptor so it can be preserved on reboots.
 */
struct terminal *shell_find(const char *name)
{
	struct terminal *t;
	int l = strlen(name) + 1;

	for (t = lps->allterm; t; t = t->next) {
		if (!strcmp(name, t->name))
			return t;
	}
	t = calloc(1, sizeof(*t) + l);
	if (!t) {
		DBG(0, "could not allocate session for %s\n", name);
		return t;
	}
	strcpy(t->name, name);
	t->the_shell = term_new("/bin/sh", t->name, (lps->fb->pixmap.height-2*lps->yofs)/lps->fontheight, 
            (lps->fb->pixmap.width-lps->xofs)/lps->fontwidth, lps->sb_lines, term_dead);
    lps->sb_step = (lps->fb->pixmap.height-2*lps->yofs)/lps->fontheight/2;
	if (!t->the_shell) {
		free(t);
		return NULL;
	}
	t->next = lps->allterm;
	lps->allterm  = t;
	return t;
}


/*
 * prepare for a restart or for final exiting
 */
static void launchpad_deinit(int restart)
{
	DBG(0, "called, restart %d\n", restart);
	lps->got_signal = 0 ;
	// XXX should remove the pending sessions from the scheduler ?
	curterm_end();

	signal(SIGINT, SIG_DFL) ;
	signal(SIGTERM, SIG_DFL) ;
	signal(SIGHUP, SIG_DFL) ;

	if (!restart)
		free_terminals();
	fd_close(&lps->kpad.fdin);
	fd_close(&lps->fw.fdin);
	fd_close(&lps->vol.fdin);
	fd_close(&lps->special.fdin);
	fd_close(&lps->kpad.fdout);
	fd_close(&lps->fw.fdout);
	fd_close(&lps->vol.fdout);
	fd_close(&lps->special.fdout);
	if (lps->db) {
		cfg_free(lps->db) ;
		lps->db = NULL ;
	}
}

int launchpad_start(void);

/*
 * callback for select.
 * We have only one session so ignore _s
 */
int handle_launchpad(void *_s, struct cb_args *a)
{
	int fds[3] = { lps->kpad.fdin, lps->fw.fdin, lps->vol.fdin };
	int i, j, ev;

	DBG(2, "fds %d %d %d\n", lps->kpad.fdin, lps->fw.fdin, lps->vol.fdin);
	DBG(2, "term %p sh %p \n",
		lps->fb, lps->curterm);
	if (lps->kpad.fdin < 0) { /* dead */
		if (a->run == 0)
			return 0;
		/* try to restart or terminate ? */
		launchpad_deinit(0);
		return 1;
	}
	if (a->run == 0) {
		/* create screen refresh timeout if needed */
		if (lps->fb && lps->curterm &&
			    term_state(lps->curterm->the_shell, NULL) &&
			    !timerisset(&lps->screen_due))
			timeradd_ms(&a->now, lps->refresh_delay, &lps->screen_due);
		/* Record pending timeouts */
		timersetmin(&a->due, &lps->screen_due);
		/* if we have keys to send, ignore input events */
		for (i=0; i < sizeof(fds)/sizeof(fds[0]); i++) {
			if (fds[i] >= 0)
				FD_SET(fds[i], a->r);
			if (fds[i] > a->maxfd)
				a->maxfd = fds[i];
		}
		return 1;
	}

	if (lps->got_signal == 1) {
		launchpad_deinit(1);
		launchpad_start();
		return 0;
	}
	if (lps->got_signal == 2) {
		launchpad_deinit(0);
		return 0;
	}
	ev = 0;
	if (1) {
		struct input_event kbbuf[2];
		for (j = 0; j < sizeof(fds) / sizeof(fds[0]) ; j++) {
			int l = sizeof(struct input_event);
			int n;
			if (fds[j] < 0 || !FD_ISSET(fds[j], a->r))
				continue;
			ev = 1;	/* got an event */
			DBG(1, "reading on %d\n", fds[j]);
			n = read(fds[j], kbbuf, l* 2) ;
			DBG(2, "got %d bytes from %d\n", n, fds[j]);
			for (i = 0; i < 2 && n >= l; i++, n -= l) {
				process_event(kbbuf + i, j) ;
			}
		}
        if (read(lps->special.fdin, kbbuf, sizeof(struct input_event))>0) {
            process_event(kbbuf, -3); /* special mode */
        }
	}
	if (timerdue(&lps->screen_due, &a->now)) {
		process_screen();
		return 0;
	}
	if (ev == 0) { /* timeout ? resync ? */
		process_event(NULL, 0);
	}
	return 0;
}

int launchpad_parse(int *ac, char *av[])
{
	int  i ;
	lps = __me.app->data;

	DBG(0, "Launchpad start routine\n");
	memset(lps, 0, sizeof(*lps));

	/* copy the full config name into lps->basedir */
	i = sizeof(lps->basedir) - strlen(".ini") - 1;
	if (readlink ("/proc/self/exe", lps->basedir, i) == -1)
		strncpy(lps->basedir, av[0], i) ;
	strcat(lps->basedir, ".ini");

	/* dirname() may or may not truncates the argument, so we
	 * enforce the truncation and append a '/'
	 */
	i = strlen( dirname(lps->basedir) );
	lps->basedir[i] = '\0';
	lps->cfg_name = lps->basedir + i + 1;
	lps->cfg_name = "myts.ini"; 
    DBG(1, "inipath is %s ini_name %s\n",
		lps->basedir, lps->cfg_name);
	if (*ac > 2 && !strcmp(av[1], "--cfg"))
		lps->cfg_name = av[2];

	return 0;
}

int launchpad_start(void)
{
	new_sess(0, -2, handle_launchpad, NULL);
	signal(SIGINT, int_handler);
	signal(SIGTERM, int_handler);
	signal(SIGHUP, hup_handler);
	process_event(NULL, 0);	/* reset args */
	if (!launchpad_init(NULL))
		return 0;
	DBG(0, "init routine failed, exiting\n");
	launchpad_deinit(0) ;
	return 0 ;
}

struct app lpad = { .parse = launchpad_parse,
	.start = launchpad_start, .data = &lpad_desc};
