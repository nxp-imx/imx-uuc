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

clean:
	rm -f uuc
	rm -f sdimage
