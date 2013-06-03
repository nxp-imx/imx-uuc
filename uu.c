/*
 * iMX utp decode program
 *
 * Copyright 2010-2013 Freescale Semiconductor, Inc.
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
#include <stdio.h>
#include <stdlib.h>
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
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <linux/reboot.h>
#include <sys/reboot.h>
/* mxc SoC will enable watchdog at USB recovery mode
 * so, the user must service watchdog
 */
#include <linux/watchdog.h>

#define UTP_DEVNODE 	"/dev/utp"
#define UTP_TARGET_FILE	"/tmp/file.utp"

#define UTP_FLAG_COMMAND	0x00000001
#define UTP_FLAG_DATA		0x00000002
#define UTP_FLAG_STATUS		0x00000004    //indicate an error happens
#define UTP_FLAG_REPORT_BUSY	0x10000000


#pragma pack(1)

#define PACKAGE "uuc"
#define VERSION "0.4"

char *utp_firmware_version = "2.6.31";
char *utp_sn = "000000000000";
char *utp_chipid = "370000A5";
/* for utp ioctl */
#define UTP_IOCTL_BASE  'U'
#define UTP_GET_CPU_ID  _IOR(UTP_IOCTL_BASE, 0, int)

#define NEED_TO_GET_CHILD_PID 1
/*
 * this structure should be in sync with the same in
 * $KERNEL/drivers/usb/gadget/fsl_updater.c
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

static int utp_file = -1;
static FILE *utp_file_f = NULL;

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
	return 0;
}
/*
 * Put the command which needs to send busy first
 * And the host will send poll for getting its return value
 * later, we call these kinds of commands as Asynchronous Commands.
 */
static int utp_can_busy(char *command)
{
	char *async[] ={
		"$ ", "frf", "pollpipe", NULL,
	};
	char **ptr;

	ptr = async;
	while (*ptr) {
		if (strncmp(command, *ptr, strlen(*ptr)) == 0)
			return 1;
		ptr++;
	}
	return 0;
}
#ifdef NEED_TO_GET_CHILD_PID
/* for pipe */
#define READ 0
#define WRITE 1
static pid_t child_pid = -1;
static int utp_flush(void)
{
	int pstat;
	int ret = 0;
	pid_t pid;
	if (utp_file >= 0) {
		fflush(NULL);
		ret = close(utp_file);
		do{
			pid = waitpid(child_pid, &pstat, 0); /* wait for child finished */
		}while (pid == -1 && errno == EINTR);
		printf("UTP: closing the file\n");
	}
	utp_file = -1;
	return ret;
}
pid_t popen2(const char *command, int *infp, int *outfp)
{
	int p_stdin[2], p_stdout[2];
	pid_t pid;

	if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0)
		return -1;

	pid = fork();

	if (pid < 0)
		return pid;
	else if (pid == 0){
		close(p_stdin[WRITE]);
		if (infp == NULL)
			close(p_stdin[READ]);
		else
			dup2(p_stdin[READ], READ);
		close(p_stdout[READ]);
		if (outfp == NULL)
			close(p_stdout[WRITE]);
		else
			dup2(p_stdout[WRITE], WRITE);

		execl("/bin/sh", "sh", "-c", command, NULL);
		perror("execl");
		exit(1);
	}

	if (infp == NULL)
		close(p_stdin[WRITE]);
	else
		*infp = p_stdin[WRITE];

	if (outfp == NULL)
		close(p_stdout[READ]);
	else
		*outfp = p_stdout[READ];

	close(p_stdin[READ]);
	close(p_stdout[WRITE]);
	return pid;
}
int utp_pipe(char *command, ... )
{
	int r, infp;
	char shell_cmd[1024];
	va_list vptr;
	va_start(vptr, command);
	vsnprintf(shell_cmd, sizeof(shell_cmd), command, vptr);
	va_end(vptr);

	child_pid = popen2(shell_cmd, &infp, NULL);
	if (child_pid < 0){
		printf("the fork is failed \n");
		return -1;
	}
	utp_file = infp;
	printf("pid is %d, UTP: executing \"%s\"\n",child_pid, shell_cmd);
	return 0;
}
int utp_poll_pipe()
{
	int ret = 0, cnt = 0xFFFF;
	while(ret == 0 && cnt > 0){
		ret = is_child_dead();
		usleep(10000);
		cnt--;
	}

	if(ret == 0)
		return 1;//failure
	else
		return 0;//Success
}


#else
static int utp_flush(void)
{
	int ret;
	if (utp_file_f) {
		printf("UTP: waiting for pipe to close\n");
		ret = pclose(utp_file_f);
	}
	else if (utp_file >= 0) {
		printf("UTP: closing the file\n");
		ret = close(utp_file);
	}
	utp_file_f = NULL;
	utp_file = -1;
	printf("UTP: files were flushed.\n");
	return ret;
}
static int utp_pipe(char *command, ... )
{
	int r;
	char shell_cmd[1024];
	va_list vptr;

	va_start(vptr, command);
	vsnprintf(shell_cmd, sizeof(shell_cmd), command, vptr);
	va_end(vptr);

	utp_file_f = popen(shell_cmd, "w");
	utp_file = fileno(utp_file_f);

	return utp_file_f ? 0 : errno;
}
#endif
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

	/* these are asynchronous commands and need to send busy */
	if (utp_can_busy(cmd)){
		utp_send_busy(u);
	}

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
		/* reboot the system, and the ACK has already sent out */
		if (cmd[1] == '3') {
			sync();
			kill(-1, SIGTERM);
			sleep(1);
			kill(-1, SIGKILL);

			reboot(LINUX_REBOOT_CMD_RESTART);
			return NULL;
		}
	}

	else if (strncmp(cmd, "$ ", 2) == 0) {
		status = utp_run(cmd + 2);
		if (status)
			flags = UTP_FLAG_STATUS;
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

	else if (strncmp(cmd, "pollpipe", 8) == 0) {
		printf("UTP: poll pipe.\n");
		status = utp_poll_pipe();
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
			printf("UTP: WARNING! payload %% 1024 != 0, the rest will be skipped");

		status = utp_pipe("dd of=%s bs=1K", devnode);
		if (status)
			flags = UTP_FLAG_STATUS;
	}


	else if (strcmp(cmd, "frf") == 0 || strcmp(cmd, "frs") == 0) {
		/* perform actual flashing of the rootfs to the NAND/SD */
		status = utp_flush();
		if (status)
			flags = UTP_FLAG_STATUS;
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
			size = lseek(f, 0, SEEK_END);	/* get the file size */
			lseek(f, 0, SEEK_SET);

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
		if (status)
			flags = UTP_FLAG_STATUS;
	}

	else {
		printf("UTP: Unknown command received, ignored\n");
		flags = UTP_FLAG_STATUS;
		status = -EINVAL;
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

/*
 * Check the process is dead
 */
#define NAME_MAX 30
int is_child_dead(void)
{
	FILE *fh;
	char path[NAME_MAX + 1];
	sprintf(path, "/proc/%u/status", (unsigned int)child_pid);
	if ((fh = fopen(path, "r"))){
		char buf[1024];
		while (fgets(buf, sizeof(buf) -1, fh)){
			if (!strncmp(buf, "State:", 6))
			{
				char *p = buf + 6;
				while (*p == '\t'){
					p++;
					continue;
				}
				if (*p == 'Z'){
					printf("Process status polling: %s is in zombie.\n",path);
					fclose(fh);
					return 1;
				}
				break;
			}
		}
	}
	else{
		printf("Process polling: can't open %s, maybe the process %u has been killed already\n",path,child_pid);
		return 1;
	}

	fclose(fh);
	return 0;
}


void feed_watchdog(void *arg)
{
	int res;
	int *fd = arg;
	while(1) {
		res = ioctl(*fd, WDIOC_KEEPALIVE);
		if (res)
			printf("ioctl WDIOC_KEEPALIVE error L%d, %s\n", __LINE__, strerror(errno));
		printf("%s\n", __func__);
		sleep(60);
	}
}

int main(void)
{
	int u = -1, wdt_fd = -1, r, need_watchdog = 0;
	int watchdog_timeout = 127;  /* sec */
	int cpu_id = 50;
	struct utp_message *uc, *answer;
	char env[256];
	pthread_t a_thread;
	void *thread_result;

	printf("%s %s [built %s %s]\n", PACKAGE, VERSION, __DATE__, __TIME__);
	/* set stdout unbuffered, what is the usage??? */
//	setvbuf(stdout, NULL, _IONBF, 0);
	uc = malloc(sizeof(*uc) + 0x10000);

	mkdir("/tmp", 0777);

	setenv("FILE", UTP_TARGET_FILE, !0);

	printf("UTP: Waiting for device to appear\n");
	while (utp_mk_devnode("class/misc", "utp", UTP_DEVNODE, S_IFCHR) < 0) {
		putchar('.');
		sleep(1);
	}
	u = open(UTP_DEVNODE, O_RDWR);
	r = ioctl(u, UTP_GET_CPU_ID, &cpu_id);
	if (r)
		printf("cpu id get error:L%d, %s\n", __LINE__, strerror(errno));
	else{
		switch (cpu_id) {
			case 23:
			case 25:
			case 28:
			case 50:
				need_watchdog = 0;
				break;
			case 35:
			case 51:
			case 53:
				need_watchdog = 1;
				break;
			default:
				need_watchdog = 0;
		}
		printf("cpu_id is %d\n", cpu_id);
		if (need_watchdog){
			if (utp_mk_devnode("class/misc", "watchdog", "/dev/watchdog", S_IFCHR)){
				printf("The watchdog is not configured, needed by mx35/mx51/mx53 \n");
				printf("%d, %s\n", __LINE__, strerror(errno));
			} else{
				wdt_fd = open("/dev/watchdog", O_RDWR);
				/* set the MAX timeout */
				r = ioctl(wdt_fd, WDIOC_SETTIMEOUT, &watchdog_timeout);
				if (r)
					printf("%d, %s\n", __LINE__, strerror(errno));
				r = pthread_create(&a_thread, NULL, (void *)feed_watchdog, (void *)(&wdt_fd));
				if (r != 0) {
					perror("Thread creation failed");
					exit(EXIT_FAILURE);
				}
			}
		}
	}

	for(;;) {
		r = read(u, uc, sizeof(*uc) + 0x10000);
		if (uc->flags & UTP_FLAG_COMMAND) {
			answer = utp_handle_command(u, uc->command, uc->payload);
			if (answer) {
				printf("UTP: sending %s to kernel for command %s.\n", utp_answer_type(answer), uc->command);
				write(u, answer, answer->size);
				free(answer);
			}
		}else if (uc->flags & UTP_FLAG_DATA) {
			write(utp_file, uc->data, uc->bufsize);
		}else {
			printf("UTP: Unknown flag %x\n", uc->flags);
		}
	}

	/* should never be here */
	return 0;
}

