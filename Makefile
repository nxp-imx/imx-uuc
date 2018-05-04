CC ?= $(CROSS_COMPILE)gcc
BINDIR ?= /usr/bin
PROGRAMS = uuc sdimage ufb
LIBS ?= -lpthread

all: $(PROGRAMS)

uuc: uu.c
	$(CC) $(CFLAGS) $(CPPFLAGS) uu.c -o uuc $(LDFLAGS) $(LIBS) 

sdimage: sdimage.c
	$(CC) $(CFLAGS) $(CPPFLAGS) sdimage.c -o sdimage $(LDFLAGS)

ufb: ufb.c
	$(CC) $(CFLAGS) $(CPPFLAGS) ufb.c -o ufb $(LDFLAGS) $(LIBS)

install:
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 linuxrc $(DESTDIR)
	install -m 755 $(PROGRAMS) $(DESTDIR)$(BINDIR)
	dd if=/dev/zero of=$(DESTDIR)/fat bs=1M count=1
	mkfs.vfat $(DESTDIR)/fat

clean:
	rm -f $(PROGRAMS)
