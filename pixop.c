/*
 * (C) 2010 Andy
 *
 * pixmap routines
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
//#include <linux/fb.h>
#include <sys/mman.h>
#include <signal.h>
#include <string.h>
#include <inttypes.h>

#include "myts.h"
#include "pixop.h"

/* transfer pixmap "src:sx,sy (width:height)" to pixmap "dst: dx, dy"
 * bg, if non-zero, is OR-ed to every byte in the dst region.
 * This function assumes bpp=4, sx%2=0.
 */
int pix_blt(pixmap_t* dst, int dx, int dy,
	pixmap_t* src, int sx, int sy, int width, int height, int bg)
{
    int w, i, j;
	unsigned char *dstp, *srcp;
	int dst_stride, src_stride;

	if (dst == NULL || src == NULL)
		return 0;
	if (width < 0) {
		width = src->width;
		height = src->height;
	}
	
//    c_truncate(&sx, &width, src->width);
//	c_truncate(&sy, &height, src->height);
//	c_truncate(&dx, &width, dst->width);
//	c_truncate(&dy, &height, dst->height);

	dst_stride = (dst->width + 1)/2;
	src_stride = (src->width + 1)/2;
	dstp = dst->surface + dy*dst_stride + dx/2;
	srcp = src->surface + sy*src_stride + sx/2;
    if(!(dx&1)) {
        w=width/2;
        for (i=0; i< height; i++) {
            memcpy(dstp, srcp, w);
            if (bg) {
                for (j=0; j< w; j++)
                    dstp[j] |= bg;
            }
            if(width&1) 
                dstp[w]=(dstp[w]&0x0f) | (srcp[w]&0xf0) | (bg&0xf0);
            dstp += dst_stride;
            srcp += src_stride;
        }
    } else {
        int j;
        w=(width+1)/2;
        for(i=0; i< height; i++) {
            dstp[0]=(dstp[0]&0xf0) | (srcp[0]>>4) | (bg&0x0f);
            for(j=1;j<w;j++) dstp[j]=(srcp[j-1]<<4) | (srcp[j]>>4) | bg;
            if(!(width&1))
                dstp[w]=(dstp[w]&0x0f) | (srcp[w-1]<<4) | (bg&0xf0);
            dstp += dst_stride;
            srcp += src_stride;
        }
    }
    return width*height;
}

pixmap_t * pix_alloc(int w, int h)
{
	int size = ((w+1)/2)*h + sizeof(pixmap_t) ;
	pixmap_t * p = (pixmap_t *)calloc(1, size);
	if (p) {
		p->width = w ;
		p->height = h ;
		p->surface = (unsigned char *)(p + 1);
	}

	return p ;
}

void pix_free(pixmap_t *p)
{
	if (p)
		free(p) ;
}
