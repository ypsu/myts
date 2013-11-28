/*
 * (C) 2010 Andy
 *
 * eink screen routines
 */

#ifndef _SCREEN_H_
#define _SCREEN_H_

#include "pixop.h"

/* defines below simply mimic values of fx_type from einkfb.h */
/* XXX shall we reuse them ? */
#define UMODE_MASK 11
#define UMODE_BUFISMASK 14
#define UMODE_NONE -1
#define UMODE_FLASH 20
#define UMODE_INVERT 21
#define UMODE_PARTIAL 0	// 0 XXX full and partial are the same ?
#define UMODE_FULL 1

typedef struct fbscreen {
	int fd ;
	int screensize ;
	int cur_x, cur_y;	/* for string processing */
	pixmap_t pixmap ;
	struct font *font;	/* default font */
} fbscreen_t;

fbscreen_t *fb_open(void) ;
void	fb_close(fbscreen_t *fb) ;
void	fb_update_area(fbscreen_t *fb, int mode, int x0, int y0, int x1, int y1, void *pbuf) ;
#endif
