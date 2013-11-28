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
 * $Id: dynstring.c 7958 2010-12-04 01:15:04Z luigi $
 *
 * An implementation of dynamic strings (and in general, extensible
 * data structures) inherited from the one i wrote myself for asterisk.
 *
 * Note that the internals of the dynstring are opaquE
 */

#include "dynstring.h"

#include <stdio.h>	/* vsnprintf */
#include <stdarg.h>	/* varargs */
#include <stdlib.h>
#include <memory.h>	/* bcopy */

#include <sys/types.h>

#define START_SIZE	48	// initial size
/*
 * This is the internal representation of the object -- a header
 * followed by an inline buffer. The entire chunk is malloc()ed or
 * realloc()ed as needed.
 *
 * 'len' represents the size of the buffer space, excluding the
 * space occupied by the structure.
 * Special case: len = 0 and used >0 represents a reference to
 * an external string (ds_readonly() will return true).
 * As an implementation detail, we make the last byte unavailable
 * for users so we put a NUL byte there and guarantee that strings
 * are well terminated.
 */
struct __dynstr {
        size_t len;     /* The current size of the buffer */
        size_t used;    /* Amount of space used */
        char str[0];    /* The string buffer */
};

static int
ds_readonly(dynstr s)
{
	return (s->len == 0 && s->used > 0);
}

const char *ds_data(dynstr s)
{
	const char **pp;

	if (!s)
		return "";
	if (s->len > 0)
		return s->str;
	if (s->used == 0)
		return "";
	pp = (const char **)&(s->str);
	return pp[0];
}

/*
 * Create a reference to an existing string. In this case the len
 * field is 0 and 'used' is the length of the string we point to.
 */
dynstr ds_ref(const char *p, int len)
{
	dynstr d = ds_create(sizeof(const char *));
	const char **pp = (const char **)&(d->str);
	pp[0] = p;
	d->len = 0;
	d->used = len;
	return d;
}

void *ds_free(dynstr s)
{
	if (s)
		free(s);
	return NULL;
}

int ds_len(dynstr s)
{
	return s ? s->used : 0;
}

/*
 * When returning the available space, decrement by 1 so we
 * can add a '\0' at the end without overwriting user data
 */
int ds_size(dynstr s)
{
	return s ? s->len - 1 : 0;
}

void ds_reset(dynstr buf)
{
        if (buf) {
                buf->used = 0;
                if (buf->len)
                        buf->str[0] = '\0';
        }
}

dynstr ds_create(int init_len)
{
        dynstr buf;

        buf = (dynstr)calloc(1, sizeof(*buf) + init_len);
        if (buf == NULL)
                return NULL;
 
        buf->len = init_len;
        buf->used = 0;
 
        return buf;
} 

static int dynstr_make_space(dynstr *buf, size_t new_len)
{
	dynstr newbuf;

	if (buf == NULL)
		return 0;
	if (ds_readonly(*buf))
		return -1;
        if (new_len <= (*buf)->len)
                return 0;       /* success */
	/* make it slightly larger than requested */
	if (new_len < 1000)
		new_len += new_len;
	else
		new_len += 1000;

        newbuf = (dynstr)realloc(*buf, new_len + sizeof(struct __dynstr));
        if (newbuf == NULL)
                return -1;
 
	*buf = newbuf;
        (*buf)->len = new_len;
        return 0;
}

static int __dynstr_helper(dynstr *buf, size_t max_len,
	int append, const char *fmt, va_list ap);

#define DYNSTR_BUILD_RETRY	-2
#define DYNSTR_BUILD_FAILED	-3
 
/*
 * Append to a dynamic string using a va_list
 */
#define vadsprintf(buf, max_len, fmt, ap)                \
        ({                                                              \
                int __res;                                              \
                while ((__res = __dynstr_helper(buf, max_len,          \
                        1, fmt, ap)) == DYNSTR_BUILD_RETRY) {       \
                        va_end(ap);                                     \
                        va_start(ap, fmt);                              \
                }                                                       \
                (__res);                                                \
        })
 
/*!
 * Append to a dynamic string - same as sprintf().
 */
int __attribute__ ((format (printf, 2, 3)))
dsprintf(dynstr *buf, const char *fmt, ...)
{
        int res;
        va_list ap;
 
	if (buf == NULL)
		return 0;
        va_start(ap, fmt);
        res = vadsprintf(buf, 0 /* max_len */, fmt, ap);
        va_end(ap);
 
        return res;
}

/*
 * Append a buffer to a dynamic string (and also a '\0' to ease printing).
 * If d == NULL only extend
 */
int ds_append(dynstr *buf, const void *d, int len)
{
	int need;
	if (buf == NULL)
		return 0;
	if (*buf == NULL)
		*buf = ds_create(START_SIZE);
	if (*buf == NULL)
		return DYNSTR_BUILD_FAILED;
	if (len < 0) {  /* the 'truncate' */
		need = -len + 1;
	} else {
		need = (*buf)->used + len + 1;
	}
	if (need > (*buf)->len) {
		if (dynstr_make_space(buf, need))
			return DYNSTR_BUILD_FAILED;
	}
	if (len < 0) {
		(*buf)->used = -len;
	} else {
		if (d)
			bcopy(d, (*buf)->str + (*buf)->used, len);
		(*buf)->used += len;
	}
	(*buf)->str[(*buf)->used] = '\0';
	return 0;
}

int ds_truncate(dynstr *buf, int want)
{
	if (buf && *buf && ds_readonly(*buf) && want <= (*buf)->used) {
		(*buf)->used = want;
		return 0;
	}
	return ds_append(buf, NULL, -want);
}

/* adjust used to the desired length */
int ds_adjust(dynstr *buf, int i, int recsize)
{
	int l = ds_len(*buf);
	int want = (i+1)*recsize;
	if (l < want)
		ds_truncate(buf, want);
	return 0;
}

/* remove the initial n bytes from the string, shifting up the content */
int ds_shift(dynstr d, int n)
{
	if (!d || n < 0 || n > d->used)
		return -1;
	d->used -= n;	// residual size
	if (ds_readonly(d)) {
		/* for readonly string, shift instead of move */
		const char **pp = (const char **)&(d->str);
		pp[0] += n;
	} else {
		bcopy(d->str + n, d->str, d->used);
		d->str[d->used] = '\0';
	}
	return d->used;
}

__attribute__((format (printf, 4, 0)))
static int __dynstr_helper(dynstr *buf, size_t max_len,
        int append, const char *fmt, va_list ap)
{
        int res, need;
        int offset;
	if (buf == NULL)
		return 0;
	if (*buf == NULL)
		*buf = ds_create(START_SIZE);
	if (*buf == NULL)
		return DYNSTR_BUILD_FAILED;
	offset = (append && (*buf)->len) ? (*buf)->used : 0;

        if (max_len < 0)
                max_len = (*buf)->len;  /* don't exceed the allocated space */
        /*
         * Ask vsnprintf how much space we need. Remember that vsnprintf
         * does not count the final '\0' so we must add 1.
         */
        res = vsnprintf((*buf)->str + offset, (*buf)->len - offset, fmt, ap);

        need = res + offset + 1;
        /*
         * If there is not enough space and we are below the max length,
         * reallocate the buffer and return a message telling to retry.
         */
        if (need > (*buf)->len && (max_len == 0 || (*buf)->len < max_len) ) {
                if (max_len && max_len < need)  /* truncate as needed */
                        need = max_len;
                else if (max_len == 0)  /* if unbounded, give more room for next time */
                        need += 16 + need/4;
                if (dynstr_make_space(buf, need))
                        return DYNSTR_BUILD_FAILED;
                (*buf)->str[offset] = '\0';     /* Truncate the partial write. */

                /* va_end() and va_start() must be done before calling
                 * vsnprintf() again. */
                return DYNSTR_BUILD_RETRY;
        }
        /* update space used, keep in mind the truncation */
        (*buf)->used = (res + offset > (*buf)->len) ? (*buf)->len : res + offset;

        return res;
}
