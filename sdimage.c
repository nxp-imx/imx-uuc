/*
 * Copyright (C) 2009-2010 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2018 Michael Heimpold
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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

#define max(a,b) \
	({ __typeof__ (a) _a = (a); \
	   __typeof__ (b) _b = (b); \
	_a > _b ? _a : _b; })

#ifndef ROUND_UP
#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))
#endif

#define __stringify_1(x...) #x
#define __stringify(x...)   __stringify_1(x)

/* size of a sector in bytes */
#define SECTOR_SIZE 512

/* calculate the count of required sectors for given byte size */
#define SECTOR_COUNT(x) (((x) + SECTOR_SIZE - 1) / SECTOR_SIZE)

/* The MX23 Boot ROM does blindly load from 2048 offset while the MX28
 * does parse the BCB header to known where to load the image from.
 * We start the image at 4 sectors offset so same code can be used by
 * both SoCs avoiding code duplication.
 */
#define IMAGE_OFFSET 4

/* The first image always starts at IMAGE_OFFSET defined above because of MX23
 * limitation. Second firmware image is placed after the first one, but it is
 * aligned to keep some free space after the first firmware image.
 * Reason for this is that we want to write the second image first with
 * leaving the first image intact and then write the first image.
 * This should guard at least a little bit against problems due to lost power
 * during writing one of the images.
 * Two cases needs to be considered:
 * a) The new firmware image is larger than the images which are already written.
 *    This is the good case because we would reserve more space for the first
 *    image and start writing after the start of the existing second one.
 * b) The new firmware image is smaller than the existing/installed images.
 *    In this case we would begin overwriting the tail of the first image and
 *    thus rendering it unusable. So aligning the second image is a compromise
 *    on using minimal space vs. allow minor firmware image changes.
 * It's user's task to estimate the firmware size variations and to
 * tell us the desired alignment, otherwise the following default
 * value (in kB) will be used.
 */
#define DEFAULT_IMAGE_ALIGNMENT 64

/* Partition Table Entry */
struct pte {
	uint8_t active;
	uint8_t chs_start[3];
	uint8_t type;
	uint8_t chs_end[3];
	uint32_t start;
	uint32_t count;
} __attribute__ ((packed));

#define MBR_SIGNATURE 0xAA55

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

#define BCB_SIGNATURE 0x00112233

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
void mbr_to_host(struct mbr *mbr)
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

#define DEFAULT_DEVICE "/dev/mmcblk0"

/* command line options */
const struct option long_options[] = {
	{ "alignment",        required_argument,   0, 'a' },
	{ "device",           required_argument,   0, 'd' },
	{ "firmware",         required_argument,   0, 'f' },
	{ "verbose",          no_argument,         0, 'v' },
	{ "help",             no_argument,         0, 'h' },
	/* stop condition for iterator */
	{ NULL,               0,                   0,   0 },
};

/* command line help descriptions */
const char *long_options_descs[] = {
	"align second firmware image to given offset (default: " __stringify(DEFAULT_IMAGE_ALIGNMENT) " kB)",
	"device to write firmware to (default: " DEFAULT_DEVICE ")",
	"firmware file to write",
	"be verbose in what's going on",
	"print this usage and exit",
	/* stop condition for iterator */
	NULL
};

void usage(const char *progname, int exitcode)
{
	const char **desc = long_options_descs;
	const struct option *op = long_options;

	fprintf(stderr,
	        "%s -- tool to install i.MX23/28 bootstreams in devices or image files\n\n"
	        "Usage: %s [options] -f <firmware>\n\n"
	        "Options:\n",
	        progname, progname);
	while (op->name && desc) {
		fprintf(stderr, "\t-%c, --%-12s\t%s\n", op->val, op->name, *desc);
		op++; desc++;
	}
	fprintf(stderr, "\n");

	exit(exitcode);
}

int parse_alignment(const char *arg)
{
	long int value;
	char *endptr;

	value = strtol(arg, &endptr, 0);
	if (*endptr) {
		fprintf(stderr, "Error: garbage after alignment value: %s\n", endptr);
		exit(EXIT_FAILURE);
	}

	return value;
}

int main(int argc, char *argv[])
{
	char *devicename = DEFAULT_DEVICE;
	char *firmware = NULL;
	struct mbr mbr;
	struct pte *part;
	struct bcb bcb;
	int rv = EXIT_FAILURE;
	int dev_fd = -1, fw_fd = -1;
	struct stat fw_stat;
	char *fw = NULL;
	int i, mincount, sector_offset = max(SECTOR_COUNT(sizeof(struct bcb)), IMAGE_OFFSET);
	int image_alignment = DEFAULT_IMAGE_ALIGNMENT; /* in kB */
	int offset;
	int verbose = 0;

	while (1) {
		int c = getopt_long(argc, argv, "a:d:f:vh", long_options, NULL);

		/* detect the end of the options */
		if (c == -1)
			break;

		switch (c) {
			case 'a':
				image_alignment = parse_alignment(optarg);
				if (image_alignment < 0) {
					fprintf(stderr, "Warning: invalid alignment '%s' given, using " __stringify(DEFAULT_IMAGE_ALIGNMENT) " instead.\n", optarg);
					image_alignment = DEFAULT_IMAGE_ALIGNMENT;
				}
				break;
			case 'd':
				devicename = optarg;
				break;
			case 'f':
				firmware = optarg;
				break;
			case 'v':
				verbose = 1;
				break;
			case 'h':
			case '?':
				rv = EXIT_SUCCESS;
				/* fall-through */
			default:
				usage(argv[0], rv);
		}
	}

	if (!firmware)
		usage(argv[0], rv);

	/* open firmware file and memory map it */
	fw_fd = open(firmware, O_RDONLY);
	if (fw_fd == -1) {
		fprintf(stderr, "Can't open firmware '%s': %s\n", firmware, strerror(errno));
		goto close_out;
	}

	if (fstat(fw_fd, &fw_stat) == -1) {
		fprintf(stderr, "fstat(%s) failed: %s\n", firmware, strerror(errno));
		goto close_out;
	}

	fw = (char *)mmap(NULL, fw_stat.st_size, PROT_READ, MAP_PRIVATE, fw_fd, 0);
	if (fw == MAP_FAILED) {
		fprintf(stderr, "mmap(%s) failed: %s\n", firmware, strerror(errno));
		goto close_out;
	}

	if (verbose) {
		printf("Firmware size: %ld bytes, %ld sectors\n", fw_stat.st_size, SECTOR_COUNT(fw_stat.st_size));
	}

	/* open target device and read MBR with partition table */
	dev_fd = open(devicename, O_RDWR);
	if (dev_fd == -1) {
		fprintf(stderr, "Can't open device '%s': %s\n", devicename, strerror(errno));
		goto close_out;
	}

	if (read(dev_fd, &mbr, sizeof(mbr)) < sizeof(mbr)) {
		fprintf(stderr, "Could not read MBR and partition table of '%s': %s", devicename, strerror(errno));
		goto close_out;
	}

	/* partition table is little endian on disk, so convert to host byte order */
	mbr_to_host(&mbr);

	/* safety check that we found a partition table at all */
	if (mbr.signature != MBR_SIGNATURE) {
		fprintf(stderr, "MBR signature check failed: expected 0x%" PRIx16 ", read 0x%" PRIx16 "\n",
		        MBR_SIGNATURE, mbr.signature);
		goto unmap_out;
	}

	/* search bootstream partition */
	for (i = 0; i < 4; i++) {
		if (mbr.partition[i].type == 'S') {
			part = &mbr.partition[i];
			if (verbose) {
				printf("Bootstream partition found: partition %d, start=%" PRIu32 " length=%" PRIu32 " (sectors)\n",
				       i, part->start, part->count);
			}
			break;
		}
	}

	if (i == 4) {
		fprintf(stderr, "Could not find bootstream partition.\n");
		goto unmap_out;
	}

	/* we assume that we want to have at least two images of the same size
	 * in the bootstream partition, plus the first sector containing the BCB
	 * combined with our desired offset to boot on i.MX23/i.MX28 likewise, plus
	 * the space we use due to image alignment;
	 * so calculate the required minimum partition size mincount (in sectors a 512 byte)
	 */
	offset = sector_offset * SECTOR_SIZE + fw_stat.st_size;
	if (image_alignment > 0)
		offset = ROUND_UP(offset, image_alignment * 1024);
	else
		offset = ROUND_UP(offset, SECTOR_SIZE);

	mincount = SECTOR_COUNT(offset + fw_stat.st_size);

	if (part->count < mincount) {
		fprintf(stderr, "Bootstream partition is too small with %" PRIu32 " sectors.\n", part->count);
		fprintf(stderr, "With two instances of this firmware and firmware alignment to %d kB,\n", image_alignment);
		fprintf(stderr, "we require at least %d sectors (or %d kB).\n", mincount, mincount * SECTOR_SIZE / 1024);
		goto unmap_out;
	}


	/* create BCB */
	memset(&bcb, 0, sizeof(bcb));
	bcb.signature = BCB_SIGNATURE;
	bcb.primary_boot_tag = 1;
	bcb.secondary_boot_tag = 2;
	bcb.num_copies = 2;

	bcb.drive_info[0].chip_num = 0;
	bcb.drive_info[0].drive_type = 0;
	bcb.drive_info[0].tag = bcb.primary_boot_tag;
	bcb.drive_info[0].first_sector_number = part->start + sector_offset;
	bcb.drive_info[0].sector_count = SECTOR_COUNT(offset) - sector_offset;

	bcb.drive_info[1].chip_num = 0;
	bcb.drive_info[1].drive_type = 0;
	bcb.drive_info[1].tag = bcb.secondary_boot_tag;
	bcb.drive_info[1].first_sector_number =
		bcb.drive_info[0].first_sector_number + bcb.drive_info[0].sector_count;
	bcb.drive_info[1].sector_count = SECTOR_COUNT(fw_stat.st_size);

	if (verbose) {
		fprintf(stderr, "1st bootstream:\n");
		fprintf(stderr, "\tstart sector: %" PRIu32 "\n", bcb.drive_info[0].first_sector_number);
		fprintf(stderr, "\tsector count: %" PRIu32 "\n", bcb.drive_info[0].sector_count);
		fprintf(stderr, "2nd bootstream:\n");
		fprintf(stderr, "\tstart sector: %" PRIu32 "\n", bcb.drive_info[1].first_sector_number);
		fprintf(stderr, "\tsector count: %" PRIu32 "\n", bcb.drive_info[1].sector_count);
	}

	/* convert bcb to disk byte order for writing */
	bcb_to_disk(&bcb);

	if (verbose) {
		printf("Updating BCB... ");
	}

	lseek(dev_fd, part->start * SECTOR_SIZE, SEEK_SET);
	if (write(dev_fd, &bcb, sizeof(bcb)) != sizeof(bcb)) {
		if (verbose) {
			printf("failed: %s\n", strerror(errno));
		} else {
			fprintf(stderr, "Writing BCB to '%s' failed: %s\n", devicename, strerror(errno));
		}
		goto unmap_out;
	} else {
		if (verbose) {
			printf("ok.\n");
		}
	}

	if (fsync(dev_fd) == -1) {
		fprintf(stderr, "fsync(%s) failed: %s\n", devicename, strerror(errno));
		goto unmap_out;
	}

	/* convert bcb back to host byte order */
	bcb_to_host(&bcb);

	printf("Writing second firmware... ");

	lseek(dev_fd, bcb.drive_info[1].first_sector_number * SECTOR_SIZE, SEEK_SET);
	if (write(dev_fd, fw, fw_stat.st_size) != fw_stat.st_size) {
		printf("failed: %s\n", strerror(errno));
		goto unmap_out;
	} else {
		if (fsync(dev_fd) == -1) {
			fprintf(stderr, "fsync(%s) failed: %s\n", devicename, strerror(errno));
			goto unmap_out;
		}

		printf("ok.\n");
	}

	printf("Writing first firmware... ");

	lseek(dev_fd, bcb.drive_info[0].first_sector_number * SECTOR_SIZE, SEEK_SET);
	if (write(dev_fd, fw, fw_stat.st_size) != fw_stat.st_size) {
		printf("failed: %s\n", strerror(errno));
		goto unmap_out;
	} else {
		if (fsync(dev_fd) == -1) {
			fprintf(stderr, "fsync(%s) failed: %s\n", devicename, strerror(errno));
			goto unmap_out;
		}

		printf("ok.\n");
	}

	rv = EXIT_SUCCESS;

unmap_out:
	munmap(fw, fw_stat.st_size);

close_out:
	if (dev_fd != -1)
		close(dev_fd);
	if (fw_fd != -1)
		close(fw_fd);

	return rv;
}
