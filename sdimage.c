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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

char *g_filedev;
char *g_firmware;

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

struct DeviceInfo {
	unsigned int u32ChipNum;
	unsigned int u32DriverType;
	unsigned int u32Tag;
	unsigned int u32FirstSectorNumber;
	unsigned int u32SectorCount;
} __attribute__ ((packed));

struct ConfigBlock {
	unsigned int u32Sigature;
	unsigned int u32PrimaryBootTag;
	unsigned int u32SecondaryBootTags;
	unsigned int u32NumCopies;
	struct DeviceInfo aDriverInfo[10];
} __attribute__ ((packed));

int main(int argc, char **argv)
{
	int i;
	int devhandle;
	int firmwarehandle;
	struct mbr mbr;
	struct ConfigBlock bcb;
	char *buff;
	struct stat filestat;
	int mincount;

	if (argc < 2) {
		printf("sdimage -f <firmware.sb> -d </dev/mmcblk>\n");
		return -1;
	}

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0) {
			g_firmware = argv[i + 1];
			i++;
		}
		if (strcmp(argv[i], "-d") == 0) {
			g_filedev = argv[i + 1];
			i++;
		}
	}

	if (g_firmware == NULL) {
		printf("you need give -f <firmware file>\n");
		return -1;
	}

	if (g_filedev == NULL) {
		printf("you need give -d <dev file> \n");
		return -1;
	}

	devhandle = open(g_filedev, O_RDWR);
	if (devhandle < 0) {
		printf("can't open file %s\n", g_filedev);
		return -1;
	}

	firmwarehandle = open(g_firmware, O_RDONLY);
	if (firmwarehandle < 0) {
		printf("can't open file %s\n", g_firmware);
		return -1;
	}

	if (stat(g_firmware, &filestat)) {
		printf("stat %s error\n", g_firmware);
		return -1;
	}

	if (read(devhandle, &mbr, sizeof(mbr)) < sizeof(mbr)) {
		printf("read block 0 fail");
		return -1;
	}

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
	bcb.u32Sigature = 0x00112233;
	bcb.u32PrimaryBootTag = 1;
	bcb.u32SecondaryBootTags = 2;
	bcb.u32NumCopies = 2;

	bcb.aDriverInfo[0].u32ChipNum = 0;
	bcb.aDriverInfo[0].u32DriverType = 0;
	bcb.aDriverInfo[0].u32Tag = bcb.u32PrimaryBootTag;
	bcb.aDriverInfo[0].u32FirstSectorNumber = mbr.partition[i].start + 4;

	bcb.aDriverInfo[1].u32ChipNum = 0;
	bcb.aDriverInfo[1].u32DriverType = 0;
	bcb.aDriverInfo[1].u32Tag = bcb.u32SecondaryBootTags;
	bcb.aDriverInfo[1].u32FirstSectorNumber =
	    bcb.aDriverInfo[0].u32FirstSectorNumber
	    + ((filestat.st_size + 511) / 512);

	lseek(devhandle, mbr.partition[i].start * 512, SEEK_SET);
	if (write(devhandle, &bcb, sizeof(bcb)) != sizeof(bcb)) {
		printf("write bcb error\n");
		return -1;
	}

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

	lseek(devhandle, bcb.aDriverInfo[0].u32FirstSectorNumber * 512, SEEK_SET);
	if (write(devhandle, buff, filestat.st_size) != filestat.st_size) {
		printf("first firmware write fail\n");
		return -1;
	}

	printf("write second firmware\n");

	lseek(devhandle, bcb.aDriverInfo[1].u32FirstSectorNumber * 512, SEEK_SET);
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
