# Compile this on an old armv6 raspberry pi.

CC=musl-gcc
STRIP=strip
CFLAGS = -static -Os -Wall -Werror
CFLAGS += -isystem /usr/lib/musl/include -isystem /usr/include
# files to publish
PUB= $(HEADERS) $(ALLSRCS) Makefile README myts myts.ini keydefs.ini $(TABLES)

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

OBJS := $(strip $(patsubst %.c,%.o,$(strip $(SRCS))))

myts: $(OBJS)
	$(CC) $(CFLAGS) -o myts $(OBJS) $(LDFLAGS)
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
	cp myts myts/myts
	zip -r myts.zip launchpad myts
	rm -r myts/ launchpad/

clean:
	rm -rf *lll myts *.o *.core *.table myts.zip

# conversion
# hexdump -e '"\n\t" 8/1 "%3d, "'
# DO NOT DELETE

%.table: codepage.sh 
	./codepage.sh $*
