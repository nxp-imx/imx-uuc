/*
 * iMX233 utp decode program
 *
 * Copyright 2008-2010 Freescale Semiconductor
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#define _GNU_SOURCE
//#include "config.h"
#include <stdio.h>
#include <unistd.h>
#include <syscall.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <stdarg.h>

#define UTP_DEVNODE 	"/tmp/utp"
#define UTP_TARGET_FILE	"/tmp/file.utp"

#define UTP_FLAG_COMMAND	0x00000001
#define UTP_FLAG_DATA		0x00000002
#define UTP_FLAG_STATUS		0x00000004
#define UTP_FLAG_REPORT_BUSY	0x10000000

#pragma pack(1)

#define PACKAGE "uuc"
#define VERSION "0.2"

char *utp_firmware_version = "2.6.26";
char *utp_sn = "000000000000";
char *utp_chipid = "370000A5";

/*
 * this structure should be in sync with the same in
 * $KERNEL/drivers/usb/gadget/stmp_updater.c
 */
struct utp_message {
	uint32_t flags;
	size_t 	 size;
	union {
		struct {
			uint64_t payload;
			char command[1];
		};
		struct {
			size_t bufsize;
			uint8_t data[1];
		};
		uint32_t status;
	};
};
#pragma pack()

int utp_file = -1;
FILE *utp_file_f;

static inline char *utp_answer_type(struct utp_message *u)
{
	if (!u)
		return "UNKNOWN";
	if (u->flags & UTP_FLAG_STATUS)
		return "Non-success";
	if (u->flags & UTP_FLAG_DATA)
		return "Data";
	if (u->flags & UTP_FLAG_REPORT_BUSY)
		return "Busy";
	if (u->flags & UTP_FLAG_COMMAND)
		return "Command ?!";
	return "Success";
}

/*
 * utp_mk_devnode
 *
 * Parse the /sys entry to find major and minor for device
 * If/when found, create device node with type 'type'
 *
 * Example: utp_mk_devnode("block", "sda", "/dev/scsi-disk-0", S_IFBLK)
 */
static int utp_mk_devnode(char *class, char *name, char *node, int type)
{
	char sys[256];
	char devnode[20]; /* major:minor */
	int major, minor;
	char *colon;
	int len, f, rc = -EINVAL;

	if (access(node, F_OK) == 0) {
		printf("UTP: file/device node %s already exists\n", node);
		return 0;
	}

	snprintf(sys, sizeof(sys), "/sys/%s/%s/dev", class, name);
	f = open(sys, O_RDONLY);
	if (f >= 0) {
		memset(devnode, 0, sizeof(devnode));
		len = read(f, devnode, sizeof(devnode));
		if (len >= 0) {
			sscanf(devnode, "%d:%d", &major, &minor);
			printf("%s: creating node '%s' with %d+%d\n", __func__, node, major, minor);
			unlink(node);
			rc = mknod(node, type | 0666, makedev(major, minor));
		}
		close(f);
	}
	return rc;
}

/*
 * utp_run
 *
 * Start the subshell and execute the command passed
 */
static utp_run(char *command, ... )
{
	int r;
	char cmd[1024];
	va_list vptr;

	va_start(vptr, command);
	vsnprintf(cmd, sizeof(cmd), command, vptr);
	va_end(vptr);

	printf("UTP: executing \"%s\"\n", cmd);
	return system(cmd);
}

/*
 * utp_send_busy
 *
 * report the busy state to the kernel state machine
 */
static void utp_send_busy(int u)
{
	struct utp_message w;

	w.flags = UTP_FLAG_REPORT_BUSY;
	w.size = sizeof(w);
	printf("UTP: sending %s\n", utp_answer_type(&w));
	write(u, &w, w.size);
}

/*
 * utp_partition_mmc
 *
 * chat with fdisk to create bootable partition of type 0x53 and extended one
 */
static int utp_partition_mmc(char *disk)
{
	char fc[50];
	int i;
	char shell_cmd[256];
	int fdisk;
	FILE *fdisk_f;

	sprintf(shell_cmd, "fdisk %s", disk);
	fdisk_f = popen(shell_cmd, "w");
	if (fdisk_f < 0)
		return errno;

	fdisk = fileno(fdisk_f);
	for (i = 4; i >= 1 ; i--) {
		sprintf(fc, "d\n%d\n", i);
		write(fdisk, fc, strlen(fc));
	}

	sprintf(fc, "n\np\n1\n1\n+16M\n");
	write(fdisk, fc, strlen(fc));

	sprintf(fc, "n\np\n2\n\n\n");
	write(fdisk, fc, strlen(fc));

	sprintf(fc, "t\n1\n0x%X\n\n", 0x53);
	write(fdisk, fc, strlen(fc));

	write(fdisk, "w\nq\n", 2);

	pclose(fdisk_f);

	return 0;
}

/*
 * utp_do_selftest
 *
 * perform some diagnostics
 *
 * TBW
 */
static int utp_do_selftest(void)
{
	printf("UTP: Self-testing\n");
	return 0;
}

static int utp_can_busy(char *command)
{
	char *async[] ={
		"?", "!", "send", "read",
		"wrf", "wff", "wfs", "wrs",
		"untar.","pipe", NULL,
	};
	char **aptr;

	aptr = async;
	while (*aptr) {
		if (strncmp(command, *aptr, strlen(*aptr)) == 0)
			return 0;
		aptr++;
	}
	return 1;
}

static void utp_flush(void)
{
	if (utp_file_f) {
		printf("UTP: waiting for pipe to close\n");
		pclose(utp_file_f);
	}
	else if (utp_file) {
		printf("UTP: closing the file\n");
		close(utp_file);
	}
	utp_file_f = NULL;
	utp_file = 0;
	printf("UTP: files were flushed.\n");
}

static int utp_pipe(char *command, ... )
{
	int r;
	char shell_cmd[1024];
	va_list vptr;

	va_start(vptr, command);
	vsnprintf(shell_cmd, sizeof(shell_cmd), command, vptr);
	va_end(vptr);

	printf("UTP: executing \"%s\"\n", shell_cmd);
	utp_file_f = popen(shell_cmd, "w");
	utp_file = fileno(utp_file_f);

	return utp_file_f ? 0 : errno;
}

/*
 * utp_handle_command
 *
 * handle the command from MSC driver
 * command can be:
 * 	?
 *	!<type>
 *	$ <shell_command>
 *	wfs/wff <X>		write firmware to SD/flash
 *	wrs/wrf <X>		write rootfs to SD/flash
 *	frs/frf <X>		format partition for root on SD/flash
 *	erase <X>		erase partition on flash
 *	read			not implemented yet
 *	write			not implemented yet
 */
static struct utp_message *utp_handle_command(int u, char *cmd, unsigned long long payload)
{
	struct utp_message *w = NULL;
	char devnode[50];	/* enough to fit /dev/mmcblk0p99    */
	char sysnode[50];	/*   -"-    -"-  mmcblk0/mmcblk0p99 */
	uint32_t flags, status;
	char *data = NULL;
	int f;
	size_t size;

	printf("UTP: received command '%s'\n", cmd);

	/* defaults */
	status = 0;
	flags = 0;
	size = 0;

	/* these are synchronous or does not require any answer */
	if (utp_can_busy(cmd))
		utp_send_busy(u);

	if (strcmp(cmd, "?") == 0) {
		/* query */
		flags = UTP_FLAG_DATA;
		data = malloc(256);
		sprintf(data,
			"<DEVICE>\n"
			" <FW>%s</FW>\n"
			" <DCE>%s</DCE>\n"
			" <SN>%s</SN>"
			" <CID>%s</CID>"
			" <VID>%04X</VID>"
			" <PID>%04X</PID>"
			"</DEVICE>\n", utp_firmware_version, VERSION, utp_sn, utp_chipid, 0x66F, 0x37FF);
		size = (strlen(data) + 1 ) * sizeof(data[0]);
	}

	else if (cmd[0] == '!') {
		/* reboot */
		if (cmd[1] == '3') {
			utp_run("reboot");
			return NULL;
		}
		utp_run("shutdown -h now");
	}

	else if (strncmp(cmd, "$ ", 2) == 0) {
		utp_run(cmd + 2);
	}

	else if (strcmp(cmd, "flush") == 0) {
		utp_flush();
	}

	else if ((strcmp(cmd,"wff") == 0) || (strcmp(cmd, "wfs") == 0)) {
		/* Write firmware - to flash or to SD, no matter */
		utp_file = open(UTP_TARGET_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0666);
	}

	else if (strcmp(cmd, "fff") == 0) {
		/* perform actual flashing of the firmware to the NAND */
		utp_flush();
		if (utp_mk_devnode("class/mtd", "mtd1", "/dev/mtd1", S_IFCHR) >= 0 &&
  		    utp_mk_devnode("class/mtd", "mtd0", "/dev/mtd0", S_IFCHR) >= 0) {
			utp_run("kobs-ng -v -d %s", UTP_TARGET_FILE);
		}
	}

	else if (strcmp(cmd, "ffs") == 0) {
		/* perform actual flashing of the firmware to the SD */
		utp_flush();

		/* partition the card */
		if (!status  && utp_mk_devnode("block", "mmcblk0", "/dev/mmc", S_IFBLK) >= 0) {
			status = utp_partition_mmc("/dev/mmc");
			/* waiting for partition table to settle */
			sleep(5);
		}

		/* write data to the first partition */
		if (!status && utp_mk_devnode("block", "mmcblk0/mmcblk0p1", "/dev/mmc0p1", S_IFBLK) >= 0) {
			utp_run("dd if=/dev/zero of=/dev/mmc0p1 bs=512 count=4");
			utp_run("dd if=%s of=/dev/mmc0p1 ibs=512 seek=4 conv=sync,notrunc", UTP_TARGET_FILE);
		}

		if (status)
			flags = UTP_FLAG_STATUS;
	}

	else if (strncmp(cmd, "mknod", 5) == 0) {
		int devtype = S_IFCHR;
		char *class, *item, *type, *node;

		class = strtok(cmd + 6, " \t,;");
		printf("class = '%s'\n", class);
		item = strtok(NULL, " \t,;");
		printf("item = '%s'\n", item);
		node = strtok(NULL, " \t,;");
		printf("node = %s\n", node);
		type = strtok(NULL, " \t,;");
		printf("type = %s\n", type);
		if (!node && item)
			sprintf(devnode, "/dev/%s", item);
		else
			strcpy(devnode, node);
		if (type && (strcmp(type, "block") == 0 || strcmp(type, "blk") == 0))
			devtype = S_IFBLK;
		printf("UTP: running utp_mk_devnode(%s,%s,%s,0x%x)\n",
				class, item, devnode, devtype);
		status = utp_mk_devnode(class, item, devnode, devtype);
		if (status)
			flags = UTP_FLAG_STATUS;
	}

	else if (strncmp(cmd, "wrf", 3) == 0) {
		/* Write rootfs to flash */
		printf("UTP: writing rootfs to flash, mtd #%c, size %lld\n",
				cmd[3], payload);

		/* ensure that device node is here  */
		snprintf(devnode, sizeof(devnode), "/dev/mtd%c", cmd[3]);
		utp_mk_devnode("class/mtd", devnode + 5, devnode, S_IFCHR);

		/* then start ubiformat and redirect its input */
		status = utp_pipe("ubiformat %s -f - -S %lld", devnode, payload);
		if (status)
			flags = UTP_FLAG_STATUS;
	}

	else if (strncmp(cmd, "pipe", 4) == 0) {
		status = utp_pipe(cmd + 5);
		if (status)
			flags = UTP_FLAG_STATUS;
	}

	else if (strncmp(cmd, "wrs", 3) == 0) {
		/* Write rootfs to the SD */
		printf("UTP: writing rootfs to SD card, mmc partition #%c, size %lld\n",
				cmd[3], payload);

		/* ensure that device node is here  */
		snprintf(devnode, sizeof(devnode), "/dev/mmcblk0p%c", cmd[3]);
		snprintf(sysnode, sizeof(sysnode), "mmcblk0/mmcblk0p%d", cmd[3]);
		utp_mk_devnode("block", sysnode, devnode, S_IFBLK);

		if (payload % 1024)
			printf("UTP: WARNING! payload % 1024 != 0, the rest will be skipped");

		status = utp_pipe("dd of=%s bs=1K", devnode);
		if (status)
			flags = UTP_FLAG_STATUS;
	}


	else if (strcmp(cmd, "frf") == 0 || strcmp(cmd, "frs") == 0) {
		/* perform actual flashing of the rootfs to the NAND/SD */
		utp_flush();
		/* done :) */
	}

	else if (strncmp(cmd, "untar.", 6) == 0) {
		status = utp_pipe("tar %cxv -C %s", cmd[6], cmd + 8);
		if (status)
			flags = UTP_FLAG_STATUS;
	}


	else if (strncmp(cmd, "read", 4) == 0) {
		f = open(cmd + 5, O_RDONLY);
		if (f < 0) {
			flags = UTP_FLAG_STATUS;
			status = errno;
		} else {
			size = lseek(f, SEEK_END, 0);	/* get the file size */
			lseek(f, SEEK_SET, 0);

			data = malloc(size);
			if (!data) {
				flags = UTP_FLAG_STATUS;
				status = -ENOMEM;
			} else {
				read(f, data, size);
				flags = UTP_FLAG_DATA;
			}
		}
	}

	else if (strcmp(cmd, "send") == 0) {
		utp_file = open(UTP_TARGET_FILE, O_TRUNC | O_CREAT | O_WRONLY, 0666);
	}

	else if (strncmp(cmd, "save", 4) == 0) {
		close(utp_file);
		rename(UTP_TARGET_FILE, cmd + 5);
	}


	else if (strcmp(cmd, "selftest") == 0) {
		status = utp_do_selftest();
		if (status != 0)
			flags = UTP_FLAG_STATUS;
	}

	else {
		printf("UTP: Unknown command, ignored\n");
		flags = UTP_FLAG_STATUS;
		status = EINVAL;
	}

	w = malloc(size + sizeof(*w));
	if (!w) {
		printf("UTP: Could not allocate %d+%d bytes!\n", size, sizeof(*w));
		return NULL;
	}

	memset(w, 0, sizeof(*w) + size);
	w->flags = flags;
	w->size = size + sizeof(*w);
	if (flags & UTP_FLAG_DATA) {
		w->bufsize = size;
		memcpy(w->data, data, size);
	}
	if (flags & UTP_FLAG_STATUS)
		w->status = status;
	if (data)
		free(data);
	return w;
}

int main(void)
{
	int u = -1, r;
	struct utp_message *uc, *answer;
	char env[256];

	setvbuf(stdout, NULL, _IONBF, 0);

	printf("%s %s [built %s %s]\n", PACKAGE, VERSION, __DATE__, __TIME__);

	uc = malloc(sizeof(*uc) + 0x10000);

	mkdir("/tmp", 0777);

	setenv("FILE", UTP_TARGET_FILE, !0);

//	utp_run("modprobe g_file_storage");

	printf("UTP: Waiting for device to appear\n");
	while (utp_mk_devnode("class/misc", "utp", UTP_DEVNODE, S_IFCHR) < 0) {
		putchar('.');
		sleep(1);
	}
	u = open(UTP_DEVNODE, O_RDWR);

	for(;;) {
		r = read(u, uc, sizeof(*uc) + 0x10000);
		if (uc->flags & UTP_FLAG_COMMAND) {
			answer = utp_handle_command(u, uc->command, uc->payload);
			if (answer) {
				printf("UTP: sending %s\n", utp_answer_type(answer));
				write(u, answer, answer->size);
				free(answer);
			}
		}

		else if (uc->flags & UTP_FLAG_DATA) {
			/*printf("UTP: data, %d bytes\n", uc->bufsize);*/
			write(utp_file, uc->data, uc->bufsize);
		}

		else {
			printf("UTP: Unknown flag %x\n", uc->flags);
		}
	}

	/* return 0; */
}

