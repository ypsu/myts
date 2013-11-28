/*
 * (C) 2010 Andy
 *
 * routines to use the eink screen
 */

#ifndef _PIXOP_H_
#define _PIXOP_H_

typedef struct pixmap_str {
	int	width;
	int	height;
	int	bpp;	/* bits per pixel */
	unsigned char	*surface;	// 1 byte = 2 pixels
} pixmap_t;

struct font {
	int	code_first;
	int	code_last;
	int	width;
	int	height;
	int	bpp;
	unsigned char	*pixmap;
};

/* correct offset and length to the "visible" range */
static inline void c_truncate(int *ofs, int *len, int bound)
{
        if (*ofs < 0) { /* decrease size by offset */
                *len += *ofs;
                *ofs = 0;
        }
	if (*ofs > bound) {
		*len = 0;
		*ofs = bound;
	}
	if (*len <= 0)
		*len = 0;
        if (*ofs + *len > bound)
                *len = bound - *ofs;
}

int get_char_pixmap(const struct font *f, int code, pixmap_t *ppx) ;

const struct font * getfngfont(const char *path) ;
void freefngfont(const struct font *) ;

int pix_blt(pixmap_t* dst, int dx, int dy,
	pixmap_t* src, int sx, int sy, int width, int height, int bg) ;

pixmap_t * pix_alloc(int w, int h) ;
void pix_free(pixmap_t *p) ;

#endif
