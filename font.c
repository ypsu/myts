#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include "myts.h"
#include "pixop.h"
#include "font.h"

#define MAXCHARS 4096
#define FONTBLOCK 256

uint32_t *quadbits;

int bytesperchar;

struct font font_pixmap = {
        .code_first = 0,
        .code_last = 255,
        .bpp =  4,
};

#define BE2LE(x) (x << 24 | x >> 24 | (x & (uint32_t)0x0000ff00UL) << 8 | (x & (uint32_t)0x00ff0000UL) >> 8)

static void calcquadbits()
{
    unsigned int j;
    int i,k;
    for (i=0;i<256;i++) {
        j=0;
        for(k=0;k<8;k++) {
            j<<=4;
            if(i&(128>>k))j+=15;
        }
        quadbits[i]=BE2LE(j);
        DBG(2,"quadbits[%02x]=%08x\n", i, j);
    }
}

int init_font(const char *cp, const char *font, int fontheight, int fontwidth) {
    int cpf;
    int i, width;
    int charstodo;
    FILE *fontf;
    int charsdone;
    unsigned short cpb[256];
    void **ftable=NULL;
    uint8_t *fchar=NULL;

    font_pixmap.height = fontheight;
    font_pixmap.width = fontwidth;
    width = (font_pixmap.width*font_pixmap.bpp+7)/8;
    if(cp==NULL || !strcmp(cp,"UTF8")) {
        font_pixmap.code_last=0xffff;

        font_pixmap.pixmap=malloc(sizeof(void*)*65536);
        bytesperchar=2;
        ftable=(void **)font_pixmap.pixmap;
        fchar=font_pixmap.pixmap+sizeof(void*)*65536;
        charstodo=MAXCHARS;
    } else {
        font_pixmap.pixmap=calloc(1,(font_pixmap.code_last-font_pixmap.code_first+1)*
                                width*font_pixmap.height);
        bytesperchar=1;
        cpf=open(cp,O_RDONLY);
        if(cpf<0) return -1;
        read(cpf,cpb,512);
        for (i=0;i<256;i++) {
            DBG(3,"CP Table %i = %04x\n", i, cpb[i]);
        }
        close(cpf);
        charstodo=256;
    }

    fontf=fopen(font,"r");
    if(fontf<0) return -2;
    quadbits=malloc(1024);
    calcquadbits();
    charsdone=0;
    char formatstring[200] = "%04x:%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"; 
    if(fontwidth>8) {
        int i;
        for(i=7;i<strlen(formatstring);i+=4)formatstring[i]='4';
    }
    while(!feof(fontf) && charsdone<charstodo){
        char line[1024];
        int i,j;
        unsigned int pix[fontheight];

        fgets(line,1022,fontf);
        i=sscanf(line, formatstring,
                &j, 
                &pix[0], &pix[1], &pix[2], &pix[3],
                &pix[4], &pix[5], &pix[6], &pix[7],
                &pix[8], &pix[9], &pix[10], &pix[11],
                &pix[12], &pix[13], &pix[14], &pix[15],
                &pix[16], &pix[17], &pix[18], &pix[19],
                &pix[20], &pix[21], &pix[22], &pix[23],
                &pix[24], &pix[25], &pix[26], &pix[27],
                &pix[28], &pix[29], &pix[30], &pix[31]
                );
        DBG(3,"i=%d\n",i);
        if(i<fontheight+1) DBG(3, "Short font line. i=%d\n",i);
        DBG(3, "Found unicode char %04x.\n", j);
        if(i==fontheight+1) {
            int l;
            if(bytesperchar==1) {
                for(l=0;l<256;l++){
                    if(j==cpb[l]) {
                        uint8_t *p,*q;
                        int k,m;
                        charsdone++;
                        DBG(3, "Found char %04x %02x\n", j, l);
                        p=(uint8_t *)(font_pixmap.pixmap+l*width*font_pixmap.height);
                        for(k=0;k<fontheight;k++) {
                            if(fontwidth>8) {
                                q=(uint8_t *)&quadbits[(pix[k]>>8)&0xff];
                                for(m=0;m<4;m++) *p++=*q++;
                                q=(uint8_t *)&quadbits[pix[k]&0xff];
                                for(m=0;m<width-4;m++) *p++=*q++;
                            } else {
                                q=(uint8_t *)&quadbits[pix[k]];
                                for(m=0;m<width;m++) *p++=*q++;
                            }
                        }
                    }
                }
            } else {
                int k,m;
                uint8_t *q;
                if((charsdone%FONTBLOCK)==0) {
                    void *tmp;
                    long d;

                    d=fchar- (font_pixmap.pixmap+sizeof(void*)*65536);
                    tmp=realloc(font_pixmap.pixmap, sizeof(void*)*65536+
                                (charsdone/FONTBLOCK+1)*FONTBLOCK*width*font_pixmap.height);
                    if(tmp==NULL) {
                        DBG(0, "Memory allocation issue. Only %d chars used.\n", charsdone);
                        free(quadbits);
                        return 0;
                    }
                    font_pixmap.pixmap = tmp;

                    ftable=(void **)font_pixmap.pixmap;
                    fchar=font_pixmap.pixmap+sizeof(void*)*65536+d;
                }
                ftable[j]=fchar;
                for(k=0;k<fontheight;k++) {
                    if(fontwidth>8) {
                        q=(uint8_t *)&quadbits[(pix[k]>>8)&0xff];
                        for(m=0;m<4;m++) *fchar++=*q++;
                        q=(uint8_t *)&quadbits[pix[k]&0xff];
                        for(m=0;m<width-4;m++) *fchar++=*q++;
                    } else {
                        q=(uint8_t *)&quadbits[pix[k]];
                        for(m=0;m<width;m++) *fchar++=*q++;
                    }
                }
                charsdone++;
            }
        }
    }
    if(charsdone<charstodo) DBG(0, "%d chars found.\n", charsdone);
    free(quadbits);
    return 0;
}
