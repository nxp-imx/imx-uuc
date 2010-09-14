CC=$(CROSS_COMPILE)gcc
AR=$(CROSS_COMPILE)AR

all: uuc sdimage

uuc: uu.c
	$(CC)  uu.c -o uuc -lpthread

sdimage: sdimage.c
	$(CC) sdimage.c -o sdimage

install:
	mkdir -p   $(DEST_DIR)
	cp linuxrc $(DEST_DIR)
	mkdir -p   $(DEST_DIR)/usr/bin
	cp uuc	   $(DEST_DIR)/usr/bin
	cp sdimage $(DEST_DIR)/usr/bin
clean:
	rm -f uuc
	rm -f sdimage
