CC ?= $(CROSS_COMPILE)gcc
AR ?= $(CROSS_COMPILE)AR
BINDIR ?= /usr/bin

all: uuc sdimage

uuc: uu.c
	$(CC)  uu.c -o uuc -lpthread

sdimage: sdimage.c
	$(CC) sdimage.c -o sdimage

install:
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 linuxrc $(DESTDIR)
	install -m 755 uuc $(DESTDIR)$(BINDIR)
	install -m 755 sdimage $(DESTDIR)$(BINDIR)
	dd if=/dev/zero of=$(DESTDIR)/fat bs=1M count=1
	mkfs.vfat $(DESTDIR)/fat

clean:
	rm -f uuc
	rm -f sdimage
