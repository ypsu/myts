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
 * $Id: dynstring.h 7958 2010-12-04 01:15:04Z luigi $
 *
 * An implementation of dynamic strings (and in general, extensible
 * data structures) inherited from the one i wrote myself for asterisk.
 *
 * This is similar to the libsbuf that is available in FreeBSD
 *
 * USE: declare the dynamic string:	dynstr s = NULL;
 * then use as asprintf(), e.g.		dsprintf(&s, fmt, ...);
 * or, to append a chunk of bytes:	ds_append(&s, ptr, len)
 *
 * Use ds_len(s), ds_data(s), ds_reset(s), ds_free(s) to get the
 * length, data pointer, reset the content, and free the memory.
 *
 * This code has been originally designed for strings, however
 * ds_append() supports appending arbitrary chunks of bytes to
 * the structure, and in fact it is very convenient to implement
 * some form of dynamic arrays.
 */

#ifndef __DYNSTRING_H
#define __DYNSTRING_H
typedef struct __dynstr * dynstr;

/* sprintf and append bytes to a dynamic string */
int dsprintf(dynstr *s, const char *fmt, ...);

/* append a chunk of bytes to the structure */
int ds_append(dynstr *s, const void *d, int len);

/* truncate or extend to the desired size */
int ds_truncate(dynstr *s, int desired_size);

/* Adjust the array so that it includes an entry of index i
 * and size recsize (i.e. at least recsize*[i+1] bytes)
 */
int ds_adjust(dynstr *s, int i, int recsize);

/* Return a pointer to the content (or to "" if empty).
 * The function never returns NULL; use ds_len() to tell if the
 * block of memory is not allocated or otherwise empty.
 */
const char *ds_data(dynstr s);

/* return the length in bytes of the content */
int ds_len(dynstr s);		// returns the string lenght

/* return the total size of the allocated buffer */
int ds_size(dynstr s);		// returns the buffer size

/* remove the initial n bytes from the string, shifting content up */
int ds_shift(dynstr s, int n);		// returns the string lenght

/* reset the buffer to the empty string, without deallocating */
void ds_reset(dynstr s);	// resets the buffer to empty string

/* Create a dynstr with given initial size.
 * Note that the 'used' field is set to 0 so ds_len will return 0
 * Normally you don't need to call ds_create unless you want
 * to set special properties on the string such as bounded size.
 */
dynstr ds_create(int len);

/*
 * Create a dynamic string that references an external buffer.
 * The string is readonly.
 */
dynstr ds_ref(const char *base, int len);

/* frees the space used. Returns NULL for convenience */
void *ds_free(dynstr s);		// frees the space
#endif	/* __DYNSTRING_H */
