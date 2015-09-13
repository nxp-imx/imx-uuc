CC ?= $(CROSS_COMPILE)gcc
BINDIR ?= /usr/bin
PROGRAMS = uuc sdimage

all: $(PROGRAMS)

uuc: uu.c
	$(CC) $(CFLAGS) uu.c -o uuc -lpthread

sdimage: sdimage.c
	$(CC) $(CFLAGS) sdimage.c -o sdimage

install:
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 linuxrc $(DESTDIR)
	install -m 755 $(PROGRAMS) $(DESTDIR)$(BINDIR)
	dd if=/dev/zero of=$(DESTDIR)/fat bs=1M count=1
	mkfs.vfat $(DESTDIR)/fat

clean:
	rm -f $(PROGRAMS)
