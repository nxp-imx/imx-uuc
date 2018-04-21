/*
 * Copyright (C) 2009-2010 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdint.h>
#include <endian.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Partition Table Entry */
struct pte {
	uint8_t active;
	uint8_t chs_start[3];
	uint8_t type;
	uint8_t chs_end[3];
	uint32_t start;
	uint32_t count;
} __attribute__ ((packed));

/* Master Boot Record */
struct mbr {
	char bootstrap_code[446];
	struct pte partition[4];
	uint16_t signature;
} __attribute__ ((packed));

/* Drive Info Data Structure */
struct drive_info {                         /* Comments from i.MX28 RM:                    */
	uint32_t chip_num;                  /* chip select, ROM does not use it            */
	uint32_t drive_type;                /* always system drive, ROM does not use it    */
	uint32_t tag;                       /* drive tag                                   */
	uint32_t first_sector_number;       /* start sector/block address of firmware      */
	uint32_t sector_count;              /* not used by ROM                             */
} __attribute__ ((packed));

/* (maximum) elements in following drive info array
 * It's a design decision that this tool only supports two elements (at the moment)
 */
#define MAX_DI_COUNT 2

/* Boot Control Block (BCB) Data Structure */
struct bcb {                                /* (Analogous) Comments from i.MX28 RM:        */
	uint32_t signature;                 /* signature 0x00112233                        */
	uint32_t primary_boot_tag;          /* primary boot drive identified by this tag   */
	uint32_t secondary_boot_tag;        /* secondary boot drive identified by this tag */
	uint32_t num_copies;                /* num elements in drive_info array            */
	struct drive_info drive_info[MAX_DI_COUNT];
	                                    /* let drive_info array be last in this data
	                                     * structure to be able to add more drives in
	                                     * future without changing ROM code            */
} __attribute__ ((packed));

/* convert the fields of struct mbr to host byte order */
void mbr_to_hbo(struct mbr *mbr)
{
	int i;

	mbr->signature = le16toh(mbr->signature);

	for (i = 0; i < 4; i++) {
		mbr->partition[i].start = le32toh(mbr->partition[i].start);
		mbr->partition[i].count = le32toh(mbr->partition[i].count);
	}
}

/* convert the fields of struct bcb to host byte order */
void bcb_to_host(struct bcb *bcb)
{
	int i;

	bcb->signature          = le32toh(bcb->signature);
	bcb->primary_boot_tag   = le32toh(bcb->primary_boot_tag);
	bcb->secondary_boot_tag = le32toh(bcb->secondary_boot_tag);
	bcb->num_copies         = le32toh(bcb->num_copies);

	/* meanwhile we have num_copies in host byte order, so we can use it */
	for (i = 0; i < bcb->num_copies; i++) {
		bcb->drive_info[i].chip_num            = le32toh(bcb->drive_info[i].chip_num);
		bcb->drive_info[i].drive_type          = le32toh(bcb->drive_info[i].drive_type);
		bcb->drive_info[i].tag                 = le32toh(bcb->drive_info[i].tag);
		bcb->drive_info[i].first_sector_number = le32toh(bcb->drive_info[i].first_sector_number);
		bcb->drive_info[i].sector_count        = le32toh(bcb->drive_info[i].sector_count);
	}
}

/* convert the fields of struct bcb to disk byte order (little endian) */
void bcb_to_disk(struct bcb *bcb)
{
	int i;

	/* convert the array items first because we need num_copies in host byte order */
	for (i = 0; i < bcb->num_copies; i++) {
		bcb->drive_info[i].chip_num            = htole32(bcb->drive_info[i].chip_num);
		bcb->drive_info[i].drive_type          = htole32(bcb->drive_info[i].drive_type);
		bcb->drive_info[i].tag                 = htole32(bcb->drive_info[i].tag);
		bcb->drive_info[i].first_sector_number = htole32(bcb->drive_info[i].first_sector_number);
		bcb->drive_info[i].sector_count        = htole32(bcb->drive_info[i].sector_count);
	}

	bcb->signature          = htole32(bcb->signature);
	bcb->primary_boot_tag   = htole32(bcb->primary_boot_tag);
	bcb->secondary_boot_tag = htole32(bcb->secondary_boot_tag);
	bcb->num_copies         = htole32(bcb->num_copies);
}


int main(int argc, char **argv)
{
	char *filedev;
	char *firmware;
	int i;
	int devhandle;
	int firmwarehandle;
	struct mbr mbr;
	struct bcb bcb;
	char *buff;
	struct stat filestat;
	int mincount;

	if (argc < 2) {
		printf("sdimage -f <firmware.sb> -d </dev/mmcblk>\n");
		return -1;
	}

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0) {
			firmware = argv[i + 1];
			i++;
		}
		if (strcmp(argv[i], "-d") == 0) {
			filedev = argv[i + 1];
			i++;
		}
	}

	if (firmware == NULL) {
		printf("you need give -f <firmware file>\n");
		return -1;
	}

	if (filedev == NULL) {
		printf("you need give -d <dev file> \n");
		return -1;
	}

	devhandle = open(filedev, O_RDWR);
	if (devhandle < 0) {
		printf("can't open file %s\n", filedev);
		return -1;
	}

	firmwarehandle = open(firmware, O_RDONLY);
	if (firmwarehandle < 0) {
		printf("can't open file %s\n", firmware);
		return -1;
	}

	if (stat(firmware, &filestat)) {
		printf("stat %s error\n", firmware);
		return -1;
	}

	if (read(devhandle, &mbr, sizeof(mbr)) < sizeof(mbr)) {
		printf("read block 0 fail");
		return -1;
	}

	mbr_to_hbo(&mbr);

	if (mbr.signature != 0xAA55) {
		printf("Check MBR signature fail 0x%x\n", mbr.signature);
		return -1;
	}

	for (i = 0; i < 4; i++) {
		if (mbr.partition[i].type == 'S')
			break;
	}

	if (i == 4) {
		printf("Can't found boot stream partition\n");
		return -1;
	}

	/* calculate required partition size for 2 images in sectors */
	mincount = 4 + 2 * ((filestat.st_size + 511) / 512);

	if (mbr.partition[i].count < mincount) {
		printf("firmare partition is too small\n");
		return -1;
	}

	memset(&bcb, 0, sizeof(bcb));
	bcb.signature = 0x00112233;
	bcb.primary_boot_tag = 1;
	bcb.secondary_boot_tag = 2;
	bcb.num_copies = 2;

	bcb.drive_info[0].chip_num = 0;
	bcb.drive_info[0].drive_type = 0;
	bcb.drive_info[0].tag = bcb.primary_boot_tag;
	bcb.drive_info[0].first_sector_number = mbr.partition[i].start + 4;

	bcb.drive_info[1].chip_num = 0;
	bcb.drive_info[1].drive_type = 0;
	bcb.drive_info[1].tag = bcb.secondary_boot_tag;
	bcb.drive_info[1].first_sector_number =
	    bcb.drive_info[0].first_sector_number
	    + ((filestat.st_size + 511) / 512);

	/* convert bcb to disk byte order for writing */
	bcb_to_disk(&bcb);

	lseek(devhandle, mbr.partition[i].start * 512, SEEK_SET);
	if (write(devhandle, &bcb, sizeof(bcb)) != sizeof(bcb)) {
		printf("write bcb error\n");
		return -1;
	}

	/* convert bcb back to host byte order */
	bcb_to_host(&bcb);

	buff = malloc(filestat.st_size);
	if (buff == NULL) {
		printf("malloc fail\n");
		return -1;
	}

	if (read(firmwarehandle, buff, filestat.st_size) != filestat.st_size) {
		printf("read firmware fail\n");
		return -1;
	}

	printf("write first firmware\n");

	lseek(devhandle, bcb.drive_info[0].first_sector_number * 512, SEEK_SET);
	if (write(devhandle, buff, filestat.st_size) != filestat.st_size) {
		printf("first firmware write fail\n");
		return -1;
	}

	printf("write second firmware\n");

	lseek(devhandle, bcb.drive_info[1].first_sector_number * 512, SEEK_SET);
	if (write(devhandle, buff, filestat.st_size) != filestat.st_size) {
		printf("second firmware write fail\n");
		return -1;
	}
	free(buff);

	if (fsync(devhandle) == -1) {
		perror("fsync");
		return -1;
	}

	close(devhandle);
	close(firmwarehandle);
	printf("done\n");

	return 0;
}
