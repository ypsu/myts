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
 * $Id: config.h 8060 2010-12-14 01:12:22Z luigi $
 *
 * Parser for config files -- similar to the .ini files
 *
 * Each section starts with [name]
 * and has multiple "key = value" entries.
 * Whitespace is generally allowed both in keys and values,
 * a ';' starts a comment unless it is in quotes, and # on the
 * left hand side also acts as a comment marker.
 * A trivial 'include = filename' statement is also supported.
 */

#ifndef _CONFIG_H_
#define _CONFIG_H_

struct entry {
	struct entry *next;
	char *key;		/* pointer to key name */
	char *value;		/* pointer to key value */
	uint16_t len1;	/* metadata */
};

struct section;
struct config;

/*
 * various accessors.
 * As a special case, if the path in cfg_read starts with a newline,
 * then we consider it an immediate string.
 */

/* load a config file and create or extend a config */
struct config * cfg_read(const char *path, const char *base,
	struct config *old);
void cfg_free(struct config *pdb) ;
/* find a specific section. If name == NULL, return the first one,
 * then if config == NULL name is interpreted as a section and
 * we return the next entry. So iteration is
 *	for (s = cfg_find_section(cfg, NULL); s; s = cfg_find_section(NULL, (void *)s) ) 
 */
struct section *cfg_find_section(struct config *, const char *name) ;

/* find an entry. If key == NULL return the first entry,
 * then iteration can be explicit as the struct is public
 */
const struct entry *cfg_find_entry(const struct section *s, const char *key) ;

/*
 * find the value for given cfg/section. If cfg == NULL sec is
 * the pointer to the section.
 */
const char *cfg_find_val(struct config *, const char *sec, const char *key);
const char *cfg_section_name(const struct section *);

/*
 * skipws() and trimws() are generic string functions useful in other
 * places as well.
 */
char *skipws(char *p);
char *trimws(char *s, char *end);

#endif /* _CONFIG_H_ */
