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
 * a config file has a number of section marked by [section-name]
 * followed by 'key = value' lines.
 * A \\ protects the next character, while strings in ' and " are
 * also protected.
 * A ';' anywhere, or a # in the left hand side start a comment. 
 * Spaces are allowed both in key and value.
 */

#define _GNU_SOURCE	/* asprintf */
#include <fcntl.h>
#include <time.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "config.h"
extern int verbose;

#ifndef DBG
#ifdef NODEBUG
#define DBG(...)
#else
#define DBG(level, format, ...)  do {                   \
        if (verbose >= level) \
                fprintf(stderr, "[%-14.14s %4d] " format,       \
                        __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        } while(0)
#endif
#endif

struct section {
        struct section *next;
        char *name;
        struct entry *keys;     /* children */
};

/*
 * A buffer contains inline the storage for the backing file,
 * and in case we read from multiple files we need to link
 * the buffers together.
 * Individual entries (section and entry) are allocated
 * individually.
 */

struct config {
        struct config *next;    /* chain pointer */
        char *pbuf ;            /* pointer to the config file buffer (inline) */
        struct section *sections ;      /* first section */
};


/* skip leading whitespace */
char *skipws(char *p)
{
	return p ? p + strspn(p, " \t\r\n") : p;
}

/* trim trailing unescaped whitespace, including comments.
 * 'end' points to the first character after our string. If NULL, it is computed.
 * Unfortunately, to understand escape sequences one must process left to right.
 */
char *trimws(char *s, char *end)
{
	int in_quote = 0;
	char *start = s, *last_ws = NULL;
	if (end == NULL)
		end = s + strlen(s);
	if (end == s)
		return start;
	for (; s < end; s++) {
		if (*s == '\\') {
			if (s+1 < end)
				s++;
			last_ws = NULL;
		} else if (in_quote) {
			in_quote = (*s != '"' && *s != '\'');
		} else if (*s == '"' || *s == '\'') {
			in_quote = 1;
			last_ws = NULL;
		} else if (*s == ';' || (s == start && *s == '#')) {
			if (!last_ws)
				last_ws = s;
			break;
		} else if (index(" \t\r\n", *s)) {
			if (!last_ws)
				last_ws = s;
		} else
			last_ws = NULL;
	}
	if (last_ws)
		s = last_ws;
	*s = '\0';
	return start;
}

/*
 * Parse a name in key = value pair. If sep[0] == '='
 * and '=' is not found return an error.
 * Trim left and right whitespace.
 */
static char *parse_name(char **p, const char *sep)
{
	char c, *q;
	int l;

	if (!sep || !p || !*p || !**p)
		return NULL;

	q = *p = skipws(*p);
	for (l=0; q[l] && !index(sep, q[l]); l++) {
		if (q[l] == '\\' && q[l+1])
			l++;
	}
	if (sep[0] == '=' && q[l] != '=') /* looking for a key, missing = */
		return NULL;
	*p += l;
	c = q[l];
	q[l] = '\0';
	if (c)
		(*p)++;
	trimws(q, q+l);
	return q;
}


/* parse the content of a config file. We expect the buffer to be
 * persistent and writable.
 */
static int cfg_parse(struct config *db, char *p, const char *basedir)
{
	struct section *cur;
	struct entry *kcur;

	DBG(3, "start, db %p content\n%.50s\n...\n", db, p);
	cur = NULL;
	kcur = NULL;

	while (p && *p) {
		char c;
		char *start;
		char *key, *val;

		start = strsep(&p, "\n");	/* to end of line */
		start = skipws(start);	/* skip whitespace */
		trimws(start, p - 1);
		switch (*start) {
		case '\0':		/* comment line */
		case ';':		/* comment line */
		case '#':		/* comment line */
			break ;

		case '[':              /* section delimiter */
			key = ++start;	/* skip it */
			while (isalnum(*key) || index("-_", *key))
				key++;
			c = *key;
			*key = '\0';
			if (c)
				key++;
			if (c != ']') {
				DBG(0, "invalid section name %s %c\n",
					start, c);
				break;
			}
			DBG(1, "start section %s\n", start);
			cur = cfg_find_section(db, start);
			if (!cur) { /* allocate new */
				cur = calloc(1, sizeof(struct section));
				if (cur == NULL) {
					DBG(0, "cannot allocate section %s\n", start);
					return -1;
				}
				cur->next = db->sections;
				db->sections = cur;
				cur->name = start;
			}
			break;

		default:	/* it a a key/value string then */
			DBG(3, "key name pair\n");
			key = parse_name(&start, "=\r\n");
			val = key ? parse_name(&start, "\r\n") : NULL;
			DBG(2, "after parse name next p %p %d\n", p, *p);
			if (!val) {
				if (key) {
					DBG(0, "cannot parse name %s\n", start);
					return -1;
				}
				break;
			}
			DBG(1, "key [%s] val [%s]\n", key, val);
			if (!strcmp(key, "include")) {
				DBG(1, "processing include %s\n", val);
				cfg_read(val, basedir, db);
				break;
			}
			if (!cur) {
				DBG(0, "key val outside section, ignore\n");
				break;
			}
			kcur = (struct entry *)cfg_find_entry(cur, start);
			if (kcur) {
				DBG(0, "replace val %s\n", kcur->value);
				// XXX should we add ?
			} else {
				kcur = calloc(1, sizeof(struct entry));
				if (kcur == NULL) {
					DBG(0, "cannot allocate key %s\n", start);
					return -1;
				}
				kcur->next = cur->keys;
				cur->keys = kcur;
				kcur->key = key;
			}
			kcur->value = val;
			break ;
		}
	}
	DBG(1, "END db %p\n", db);
	return 0; 
}


/*
 * Creates a database for file 'path', optionally prepending 'base' to
 * the file name. If 'path' starts with newline, then this is an
 * immediate string with the content of the file.
 * If the db argument is non null then it appends to the existing db.
 */
struct config *cfg_read(const char *path, const char *base,
	struct config *old)
{
	struct config *db = NULL;
	int n, len, fd = -1;
	char *buf;

	DBG(1, "%s\n", path);
	if (path == NULL)
		return NULL;
	if (path[0] == '\n') {
		len = strlen(path);
		goto immediate;
	}
	if ( (fd = open(path, O_RDONLY)) >= 0)
		goto good;
	if (path[0] != '.' && path[0] != '/') { // try alternate location
		char *p;
		asprintf(&p, "%s/%s", base, path);
		fd = open(p, O_RDONLY);
		if ( fd >= 0)
			goto good;
	}
	DBG(0, "error opening %s\n", path);
	return old;
good:
	len = lseek(fd, 0, SEEK_END) ;
	lseek(fd, 0, SEEK_SET) ;
immediate:
	// allocate memory for descriptor + data + trailing '\0'
	db = calloc(1, sizeof(struct config) + len + 1);
	if (db == NULL)
		goto error;

	buf = db->pbuf = (char *)(db + 1);	// data are after descriptor

	// read the file
	if (fd < 0) {
		memcpy(db->pbuf, path, len);
	} else {
		n = read(fd, db->pbuf, len);
		if (n != len) {
			DBG(0, "cannot read file %s size %d\n", path, len) ;
			goto error;
		}
		close(fd) ;		/* don't need the file open anymore .. */
		fd = -1;
	}
	if (old) {	/* link the db to the next one */
		db->next = old;
		old->next = db;
		db = old;
	}
	/* do not abort if we are extending another file */
	if (!cfg_parse(db, buf, base) || old)
		return db ;
	DBG(0, "can't create db structure\n") ;

error:
	if (db)
		free(db);
	if (fd >= 0)
		close(fd) ;
	return old ;
}


/*
 * destructor for the data structure
 */
void cfg_free(struct config *p)
{
	struct section *s;
	struct entry *k;
	if (!p)
		return;
	while ( (s = p->sections) ) {
		p->sections = s->next;
		while ( (k = s->keys) ) {
			s->keys = k->next;
			free(k);
		}
		free(s);
	}
	free(p) ;
}

const char *cfg_section_name(const struct section *sec)
{
	return sec ? sec->name : NULL;
}

struct section *cfg_find_section(struct config *db, const char *name)
{
	struct section *s;
	if (db == NULL)		/* iterate, use name as current ptr */
		return name ?  ((struct section *)name)->next : NULL;
	s = db->sections;
	if (name == NULL)
		return s;
	for (; s; s = s->next)
		if (!strcasecmp(name, s->name))
			return s;
	return NULL ;
}

const struct entry * cfg_find_entry(const struct section *s, const char *name)
{
	const struct entry *k ;
	if (!s)
		return NULL;
	k = s->keys;
	if (name == NULL)
		return k;
	for (; k; k = k->next)
		if (strcasecmp(name, k->key) == 0)
			return k;
	return NULL ;
}

const char *cfg_find_val(struct config *db, const char *sec, const char *name)
{
	const struct section *s = db ? cfg_find_section(db, sec) :
		(const struct section *)sec;
	const struct entry *k = cfg_find_entry(s, name);
	return k ? k->value : NULL;
}
