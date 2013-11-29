# $Id: Makefile 8169 2011-01-07 17:32:36Z luigi $

#CC=arm-linux-gnueabi-gcc
#CC=/opt/toolchains/arm-2009q3/bin/arm-none-linux-gnueabi-gcc
CC=gcc
#STRIP=arm-linux-gnueabi-strip
#STRIP=strip
STRIP=true
#CFLAGS = -static -Os -Wall -Werror 
CFLAGS = -static -g -Wall -Werror 
CFLAGS += -nostdinc -isystem ~/proj/oss/musl-0.9.14/include/ -isystem /usr/include
# files to publish
PUB= $(HEADERS) $(ALLSRCS) Makefile README myts.arm myts.ini keydefs.ini $(TABLES)

CODEPAGES = CP437 CP1255
TABLES = $(patsubst %,%.table,$(CODEPAGES))

HEADERS = config.h dynstring.h font.h myts.h pixop.h screen.h terminal.h
HEADERS += linux/
ALLSRCS= myts.c terminal.c dynstring.c
ALLSRCS += config.c launchpad.c
ALLSRCS += screen.c pixop.c font.c
SRCS= $(ALLSRCS)
CFLAGS += -I.

CFLAGS += -DNODEBUG
LDFLAGS = ~/proj/oss/musl-0.9.14/crt/crt1.o
LDFLAGS += -nostdlib -L ~/proj/oss/musl-0.9.14/lib -lutil -lc -lgcc -lgcc_eh -lc

OBJS := $(strip $(patsubst %.c,%.o,$(strip $(SRCS))))

myts.arm: $(OBJS)
	$(CC) $(CFLAGS) -o myts.arm $(OBJS) $(LDFLAGS)
	$(STRIP) $@

$(OBJS): myts.h
terminal.o: terminal.h

tgz: $(PUB)
	tar cvzf /tmp/kiterm.tgz --exclude .svn $(PUB)

myts.zip: $(PUB)
	rm -f myts.zip
	mkdir -p myts
	mkdir -p launchpad
	cp myts.l.ini launchpad/
	cp profile myts.sh myts.ini *.hex *.table README keymap keydefs.ini bdf2hex about.txt myts/
	cp myts.arm myts/myts
	zip -r myts.zip launchpad myts
	rm -r myts/ launchpad/

clean:
	rm -rf *lll myts.arm *.o *.core *.table myts.zip

# conversion
# hexdump -e '"\n\t" 8/1 "%3d, "'
# DO NOT DELETE

%.table: codepage.sh 
	./codepage.sh $*
