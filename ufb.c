#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>

#include <linux/usb/functionfs.h>

#define PACKAGE "uuu fastboot client"
#define VERSION "1.0.0"

#define READ 0
#define WRITE 1

#define cpu_to_le16(x)  htole16(x)
#define cpu_to_le32(x)  htole32(x)
#define le32_to_cpu(x)  le32toh(x)
#define le16_to_cpu(x)  le16toh(x)

#pragma pack(1)
struct usb_fs_desc{
        struct usb_functionfs_descs_head_v2 header;
        __le32 fs_count;
        __le32 hs_count;
	__le32 ss_count;
	__le32 os_count;
        struct {
                struct usb_interface_descriptor intf;
                struct usb_endpoint_descriptor_no_audio sink;
                struct usb_endpoint_descriptor_no_audio source;
        } __attribute__((packed)) fs_descs, hs_descs;
	struct {
		struct usb_interface_descriptor intf;
		struct usb_endpoint_descriptor_no_audio sink;
		struct usb_ss_ep_comp_descriptor sink_comp;
		struct usb_endpoint_descriptor_no_audio source;
		struct usb_ss_ep_comp_descriptor source_comp;
	} __attribute__((packed)) ss_descs;
	struct usb_os_desc_header os_header;
	struct usb_ext_compat_desc os_desc;
};

#define STR_INTERFACE_ "utp"

static const struct {
        struct usb_functionfs_strings_head header;
        struct {
                __le16 code;
                const char str1[sizeof STR_INTERFACE_];
        } __attribute__((packed)) lang0;
} __attribute__((packed)) g_strings = {
        .header = {
                .magic = cpu_to_le32(FUNCTIONFS_STRINGS_MAGIC),
                .length = cpu_to_le32(sizeof g_strings),
                .str_count = cpu_to_le32(1),
                .lang_count = cpu_to_le32(1),
        },
        .lang0 = {
                cpu_to_le16(0x0409), /* en-us */
                STR_INTERFACE_,
        },
};

#pragma pack()

static const struct usb_fs_desc g_descriptors = {
        .header = {
                .magic = cpu_to_le32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
                .flags = cpu_to_le32(FUNCTIONFS_HAS_FS_DESC |
                                     FUNCTIONFS_HAS_HS_DESC |
				     FUNCTIONFS_HAS_SS_DESC |
				     FUNCTIONFS_HAS_MS_OS_DESC
				    ),
                .length = cpu_to_le32(sizeof g_descriptors),
        },
        .fs_count = cpu_to_le32(3),
        .fs_descs = {
                .intf = {
                        .bLength = sizeof g_descriptors.fs_descs.intf,
                        .bDescriptorType = USB_DT_INTERFACE,
                        .bNumEndpoints = 2,
                        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
                        .iInterface = 1,
                },
                .sink = {
                        .bLength = sizeof g_descriptors.fs_descs.sink,
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 1 | USB_DIR_IN,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                },
                .source = {
                        .bLength = sizeof g_descriptors.fs_descs.source,
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 2 | USB_DIR_OUT,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        /* .wMaxPacketSize = autoconfiguration (kernel) */
                },
        },
	.hs_count = cpu_to_le32(3),
        .hs_descs = {
                .intf = {
                        .bLength = sizeof g_descriptors.fs_descs.intf,
                        .bDescriptorType = USB_DT_INTERFACE,
                        .bNumEndpoints = 2,
                        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
                        .iInterface = 1,
                },
                .sink = {
                        .bLength = sizeof g_descriptors.hs_descs.sink,
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 1 | USB_DIR_IN,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        .wMaxPacketSize = cpu_to_le16(512),
                },
                .source = {
                        .bLength = sizeof g_descriptors.hs_descs.source,
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 2 | USB_DIR_OUT,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        .wMaxPacketSize = cpu_to_le16(512),
                        .bInterval = 1, /* NAK every 1 uframe */
                },
        },
	.ss_count = cpu_to_le32(5),
        .ss_descs = {
                .intf = {
                        .bLength = sizeof g_descriptors.ss_descs.intf,
                        .bDescriptorType = USB_DT_INTERFACE,
                        .bNumEndpoints = 2,
                        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
                        .iInterface = 1,
                },
                .sink = {
                        .bLength = sizeof g_descriptors.ss_descs.sink,
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 1 | USB_DIR_IN,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        .wMaxPacketSize = cpu_to_le16(1024),
                },
		.sink_comp = {
			.bLength = sizeof(g_descriptors.ss_descs.sink_comp),
			.bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
		},
                .source = {
                        .bLength = sizeof g_descriptors.ss_descs.source,
                        .bDescriptorType = USB_DT_ENDPOINT,
                        .bEndpointAddress = 2 | USB_DIR_OUT,
                        .bmAttributes = USB_ENDPOINT_XFER_BULK,
                        .wMaxPacketSize = cpu_to_le16(1024),
                        .bInterval = 1, /* NAK every 1 uframe */
                },
		.source_comp = {
			.bLength = sizeof(g_descriptors.ss_descs.source_comp),
			.bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
		},
        },
	.os_count = cpu_to_le32(1),
	.os_header = {
		.interface = cpu_to_le32(1),
		.dwLength = cpu_to_le32(sizeof(g_descriptors.os_header)
				        + sizeof(g_descriptors.os_desc)),
		.bcdVersion = cpu_to_le32(1),
		.wIndex = cpu_to_le32(4),
		.bCount = cpu_to_le32(1),
		.Reserved = cpu_to_le32(0),
	},
	.os_desc = {
		.bFirstInterfaceNumber = 0,
		.Reserved1 = cpu_to_le32(1),
		.CompatibleID = {'W','I','N','U','S','B',0,0},
		.SubCompatibleID = {0},
		.Reserved2 = {0},
	},
};

pid_t popen2(const char *command, int *infp, int *outfp)
{
        int p_stdin[2], p_stdout[2];
        pid_t pid;

        if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0)
                return -1;

        pid = fork();

        if (pid < 0)
                return pid;
        else if (pid == 0) {
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

#define MAX_FRAME_SIZE 64
#define MAX_FRAME_DATA_SIZE 60
union FBFrame{
	uint8_t raw[MAX_FRAME_SIZE];
	struct {
		uint32_t key;
		uint8_t  data[MAX_FRAME_SIZE - sizeof(uint32_t)];
	};
};

#define INFO (('I') | ('N' << 8) | ('F' << 16) | ('O' << 24))
#define FAIL (('F') | ('A' << 8) | ('I' << 16) | ('L' << 24))
#define OKAY (('O') | ('K' << 8) | ('A' << 16) | ('Y' << 24))
#define DATA (('D') | ('A' << 8) | ('T' << 16) | ('A' << 24))

int g_stdin  = -1;
int g_stdout = -1;
int g_pid = -1;
int g_ep_sink = -1;
int g_ep_source = -1;
int g_ep_0 = -1;
int g_open_file = -1;

int send_data(void *p, size_t size)
{
	int r;
	r = write(g_ep_sink, p, size);
	if(r < 0)
		printf("failure write to usb ep\n");

}

int handle_cmd(const char *cmd)
{
	int pid;
	int out;
	int in;
	union FBFrame fm;
	int size;
	int p;
	int pstat;
	int flags;
	struct timeval tv;
	fd_set rfds;
	tv.tv_sec = 0;
	tv.tv_usec = 50000;

	if(strncmp(cmd, "UCmd:", 5) == 0)
	{
		printf("run shell cmd: %s\n", cmd+5);
		pid = popen2(cmd + 5, NULL, &out);
		if (pid < 0) {
			printf("Failure excecu cmd: %s\n", cmd+5);
			memset(&fm, 0, sizeof(fm));
			fm.key = FAIL;
			strcpy(fm.data, "Failure to folk process");
			return -1;
		}
		memset(&fm, 0, sizeof(fm));

		flags = fcntl(out, F_GETFL);
		flags |= O_NONBLOCK;
		if (fcntl(out, F_SETFL, flags)) {
			printf("fctl failure\n");
			return -1;
		}

		FD_ZERO(&rfds);
		FD_SET(out, &rfds);
		do {
			int retval;
			p = waitpid(pid, &pstat, WNOHANG);
			retval = select(out + 1, &rfds, NULL, NULL, &tv);
			do {
				size = read(out, fm.data, MAX_FRAME_DATA_SIZE);
				if( size >= 0 ) {
					fm.key = INFO;
					send_data(&fm, size + 4);
				}
			} while(size == MAX_FRAME_DATA_SIZE);

			fm.key = INFO;
			send_data(&fm, 4);

		} while(p == 0);

		fm.key = WEXITSTATUS (pstat) ? FAIL : OKAY;
		send_data(&fm, 4);

		close(out);

	} else 	if(strncmp(cmd, "ACmd:", 5) == 0) {
                printf("run shell cmd: %s\n", cmd + 5);
		g_pid = popen2(cmd + 6, &g_stdin, &g_stdout);
		if (g_pid < 0) {
                        printf("Failure excecu cmd: %s\n", cmd+6);
                        memset(&fm, 0, sizeof(fm));
                        fm.key = FAIL;
                        strcpy(fm.data, "Failure to folk process");
		}
		fm.key = OKAY;
		g_open_file = g_stdin;
		send_data(&fm, 4);

	} else if(strncmp(cmd, "Sync", 4) == 0) {
		printf("wait for async proccess finish\n");
		FD_ZERO(&rfds);
		FD_SET(g_stdout, &rfds);
		do {
			int retval;
			p = waitpid(pid, &pstat, WNOHANG);
			if(g_stdout >= 0) {
				retval = select(g_stdout + 1, &rfds, NULL, NULL, &tv);
				do {
					size = read(g_stdout, fm.data, MAX_FRAME_DATA_SIZE);
					if( size >= 0 ) {
						fm.key = INFO;
						send_data(&fm, size + 4);
					}
				} while(size == MAX_FRAME_DATA_SIZE);
			}
			fm.key = INFO;
			send_data(&fm, 4);
		} while(p == 0);

		fm.key = WEXITSTATUS (pstat) ? FAIL : OKAY;
		send_data(&fm, 4);

		close(g_stdin);
		close(g_stdout);
		g_open_file = -1;
		g_stdin = g_stdout = -1;

	} else if(strncmp(cmd, "WOpen:", 6) == 0) {
		int rs = 4;
		printf("WOpen:%s\n", cmd + 6);
		if(cmd[6] == '-') {
			g_open_file = g_stdin;
		}
		else {
			const char *file = cmd + 6;
			struct stat st;
			if(stat(file, &st)) {
				g_open_file = open(file, O_WRONLY | O_CREAT,
					S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
			} else {
				if(st.st_mode & S_IFDIR) {
					g_open_file = -1;
					sprintf(fm.data, "%s", "DIR");
					rs = 7;
				} else {
					g_open_file = open(file, O_WRONLY | O_CREAT);
				}
			}
		}
		if (g_open_file < 0)
			fm.key = FAIL;
		else
			fm.key = OKAY;
		send_data(&fm, rs);

	} else if(strncmp(cmd, "ROpen:", 6) == 0) {
		printf("ROpen: %s\n", cmd + 6);
		size_t size = 0;
		struct stat st;
		int rz = 4;
		if(cmd[6] == '-') {
			g_open_file = g_stdout;
		} else {
			const char *file = cmd + 6;
			g_open_file = open(file, O_RDONLY);
			memset(&st, 0, sizeof(st));
			stat(file, &st);
			size = st.st_size;
			sprintf(fm.data, "%016lX", size);
			rz = 4 + strlen(fm.data);
		}

		if (g_open_file <0)
			fm.key = FAIL;
		else
			fm.key = OKAY;
		send_data(&fm, rz);

	} else if (strncmp(cmd, "Close", 5) == 0) {
		close(g_open_file);
		g_open_file = -1;
		fm.key = OKAY;
		send_data(&fm, 4);

	} else if(strncmp(cmd, "donwload:", 9) == 0) {
		uint32_t size;
		ssize_t rs;

		fm.key = OKAY;

		size = strtoul(cmd + 9, NULL, 16);

		void *p = malloc(size);
		if(p) {
			fm.key = DATA;
		} else {
			fm.key = FAIL;
			send_data(&fm, 4);
			return -1;
		}

		sprintf(fm.data, "%08X", size);
		send_data(&fm, 4 + strlen(fm.data));

		if(read(g_ep_source, p, size) < 0)
			fm.key = FAIL;

		if(write(g_open_file, p, size) < 0)
			fm.key = FAIL;

		free(p);

		memset(&fm, 0, sizeof(fm));

		if(g_stdout >= 0) {
			flags = fcntl(g_stdout, F_GETFL);
			flags |= O_NONBLOCK;
			if (fcntl(g_stdout, F_SETFL, flags)) {
				printf("fctl failure\n");
				return -1;
			}
			while((rs = read(g_stdout, fm.data, MAX_FRAME_DATA_SIZE))> 0) {
				fm.key = INFO;
				send_data(&fm, rs + 4);
			}
		}
		fm.key = OKAY;
		send_data(&fm, 4);

	} else if(strncmp(cmd, "upload", 6) == 0) {
		int max = 0x10000;
		void * p = malloc(max);
		printf(".");
		int ret  = 0;
		if (p == NULL) {
			fm.key = FAIL;
			send_data(&fm, 4);
		} else {
			ret = read(g_open_file, p, max);
			if(ret < 0) {
				fm.key = FAIL;
				send_data(&fm, 4);
			} else {
				fm.key = DATA;
				sprintf(fm.data, "%08X", ret);
				send_data(&fm, 12);
				send_data(p, ret);
				fm.key = OKAY;
				send_data(&fm, 4);
			}
		}
		free(p);
	} else {
		printf("Unknow Cmd %s\n", cmd);
	}
}


int init_usb_fs()
{
        ssize_t ret;

	printf("Start init usb\n");

        ret = write(g_ep_0, &g_descriptors, sizeof(g_descriptors));
        if ( ret < 0) {
                printf("write descriptor failure\n");
                exit(1);
        }

	printf("write string\n");
        ret = write(g_ep_0, &g_strings, sizeof(g_strings));
        if (ret < 0) {
                printf("write string failure\n");
                exit(1);
        }
}


int main(int argc, char **argv)
{
	int outfp;
	int infp;

	printf("%s %s [built %s %s]\n", PACKAGE, VERSION, __DATE__, __TIME__);

	char file[] = "/dev/usb-ffs/ep0";
	char *usb_file = file;

        if (argc > 1)
                usb_file = argv[1];

	g_ep_0 = open(usb_file, O_RDWR);
	if(g_ep_0 < 0) {
		printf("Can't open file %s\n", usb_file);
		exit(1);
	}
	init_usb_fs();

	usb_file[strlen(usb_file)-1] = '1';
        g_ep_sink = open(usb_file, O_RDWR);
        if(g_ep_sink < 0) {
		printf("can't open file %s\n", usb_file);
                exit(1);
        }

	usb_file[strlen(usb_file)-1] = '2';
	g_ep_source = open(usb_file, O_RDWR);
        if(g_ep_source < 0) {
		printf("can't open file %s\n", usb_file);
		exit(1);
	}

	printf("Start handle command\n");
	while(1) {
		int r;
		char buff[65];
		memset(buff, 0, 65);
		r = read(g_ep_source, buff, 65);
		if(r<0) printf("failure read command from usb ep point\n");
		handle_cmd(buff);
	}

	return 0;
}
