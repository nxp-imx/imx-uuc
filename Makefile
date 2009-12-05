CC=$(CROSS_COMPILE)gcc
AR=$(CROSS_COMPILE)AR

all: uuc

uuc: uu.c
	$(CC)  uu.c -o uuc

install:
	mkdir -p   $(DEST_DIR)
	cp linuxrc $(DEST_DIR)
	mkdir -p   $(DEST_DIR)/usr/bin
	cp uuc	   $(DEST_DIR)/usr/bin
clean:
	rm -f uuc
