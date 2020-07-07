#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xdamage.h>
#include <schroedinger/schro.h>

volatile int frame_ctr = 0;
unsigned int mtu = 1438;
uint8_t send_buf[4096];

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

struct client {
	unsigned int timeout;
	unsigned int addr_size;
	struct sockaddr_storage addr;
};

// TODO proper data structure
struct client clients[256];
int nclients;

void *thread_func_fps_counter(void *t) {
	while(true) {
		fprintf(stderr, "FPS: %d\n", frame_ctr);
		frame_ctr = 0;
		sleep(1);
	}
}

void *thread_func_socket_read(void *t) {
	int fd = (int) t;
	struct sockaddr_storage src_addr = {};

	char buf[4096];

	struct iovec iov[1];
	iov[0].iov_base = buf;
	iov[0].iov_len = sizeof(buf);

	struct msghdr msg;
	msg.msg_name = &src_addr;
	msg.msg_namelen = sizeof(src_addr);
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = 0;
	msg.msg_controllen = 0;

	while(true) {
		recvmsg(fd, &msg, 0);
		for(int i = 0; i < nclients; ++i) {
			if(memcmp(&clients[i].addr, &src_addr, sizeof(struct sockaddr_storage)) == 0) {
				goto outer;
			}
		}
		printf("New client\n");
		clients[nclients].timeout = 0;
		clients[nclients].addr_size = src_addr.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
		clients[nclients].addr = src_addr;
		nclients++;
outer:;
	}
}

void send_all(int fd, void *buffer, size_t size) {
	for(int i = 0; i < nclients; ++i) {
		if(sendto(fd, buffer, size, 0, (struct sockaddr *) &clients[i].addr, clients[i].addr_size) == -1) {
			perror("sendto failed");
		}
	}
}

void *thread_func_keepalive(void *t) {
	int fd = (int) t;
	while(true) {
		send_all(fd, NULL, 0);
		sleep(1);
	}
}

void report_cursor_pos(int fd, uint16_t x, uint16_t y) {
	send_buf[0] = 1;
	*(uint16_t *) (send_buf + 1) = x;
	*(uint16_t *) (send_buf + 3) = y;
	send_all(fd, send_buf, 5);
}

uint8_t cursor_buf[262144];

void report_cursor_img(Display *display, int fd) {
	XFixesCursorImage *image = XFixesGetCursorImage(display);

	size_t len = image->width * image->height * 4;

	for(int i = 0; i < image->width * image->height; ++i) {
		cursor_buf[i * 4] = image->pixels[i];
		cursor_buf[i * 4 + 1] = (image->pixels[i] >> 8 & 0xFF);
		cursor_buf[i * 4 + 2] = (image->pixels[i] >> 16 & 0xFF);
		cursor_buf[i * 4 + 3] = (image->pixels[i] >> 24 & 0xFF);
	}

	send_buf[0] = 2;
	for(size_t i = 0; i < len; i += mtu - 5) {
		*(uint32_t *) (send_buf + 1) = i;
		size_t data_len = MIN(mtu - 5, len - i);
		memcpy(send_buf + 5, cursor_buf + i, data_len);
		send_all(fd, send_buf, data_len + 5);
	}

	send_buf[0] = 3;
	*(uint16_t *) (send_buf + 1) = image->xhot;
	*(uint16_t *) (send_buf + 3) = image->yhot;
	*(uint16_t *) (send_buf + 5) = image->width;
	*(uint16_t *) (send_buf + 7) = image->height;
	send_all(fd, send_buf, 9);

	XFree(image);
}

void report_cursor_outside(int fd) {
	send_buf[0] = 4;
	send_all(fd, send_buf, 1);
}

int new_socket(char *listen_host, char *listen_port) {
	struct addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_flags = AI_PASSIVE;

	struct addrinfo *res;

	int s;
	if((s = getaddrinfo(listen_host, listen_port, &hints, &res)) != 0) {
		fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(s));
		return -1;
	}

	int fd;
	if((fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) == -1) {
		perror("socket failed");
		goto err;
	}

	if(bind(fd, res->ai_addr, res->ai_addrlen) == -1) {
		perror("bind failed");
		goto err;
	}

	freeaddrinfo(res);
	return fd;

err:
	close(fd);
	freeaddrinfo(res);

	return -1;
}

struct region {
	int x, y;
	int width, height;
};

bool clip(const struct region *reg1, const struct region *reg2, struct region *res) {
	res->x = MAX(reg1->x, reg2->x),
	res->y = MAX(reg1->y, reg2->y),
	res->width = MIN(reg1->x + reg1->width, reg2->x + reg2->width) - res->x,
	res->height = MIN(reg1->y + reg1->height, reg2->y + reg2->height) - res->y;
	return res->width > 0 && res->height > 0;
}

bool inside(int x, int y, const struct region *reg) {
	return x >= reg->x && y >= reg->y && x < reg->x + reg->width && y < reg->y + reg->height;
}

int main() {
	unsigned long long unused;

	struct region capture_region = {4480, 360, 1920, 1080};
//	struct region capture_region = {1920, 360, 1920, 1080};

//	schro_init();

//	SchroEncoder *encoder = schro_encoder_new();

//	schro_encoder_set_video_format(encoder, )


	int sockfd = new_socket("0.0.0.0", "9999");
	if(sockfd == -1) {
		fprintf(stderr, "socket creation failed\n");
		return 1;
	}

	pthread_t thread_fps_counter;
	pthread_create(&thread_fps_counter, NULL, thread_func_fps_counter, NULL);

	pthread_t thread_socket_read;
	pthread_create(&thread_socket_read, NULL, thread_func_socket_read, (void *) sockfd);

	pthread_t thread_keepalive;
	pthread_create(&thread_keepalive, NULL, thread_func_keepalive, (void *) sockfd);

	Display *display = XOpenDisplay(NULL);
	if(!display) {
		fprintf(stderr, "Cannot open display\n");
		return 1;
	}

	if(!XShmQueryExtension(display)) {
		fprintf(stderr, "No XShm extension on display\n");
		return 1;
	}

	if (!XQueryExtension(display, "XInputExtension", (int *) &unused, (int *) &unused, (int *) &unused)) {
		fprintf(stderr, "No XInput2 extension on display\n");
		return 1;
	}

	int xfixes_base_event_type;
	if(!XFixesQueryExtension(display, &xfixes_base_event_type, (int *) &unused)) {
		fprintf(stderr, "No XFixes extension on display\n");
		return 1;
	}

	int xdamage_base_event_type;
	if(!XDamageQueryExtension(display, &xdamage_base_event_type, (int *) &unused)) {
		fprintf(stderr, "No XDamage extension on display\n");
		return 1;
	}

	int screen = XDefaultScreen(display);
	Window root = RootWindow(display, screen);

	// XInput2 ask for cursor move events
	XIEventMask masks[1];
	unsigned char mask[(XI_LASTEVENT + 7)/8];

	memset(mask, 0, sizeof(mask));
	XISetMask(mask, XI_RawMotion);

	masks[0].deviceid = XIAllDevices;
	masks[0].mask_len = sizeof(mask);
	masks[0].mask = mask;

	XISelectEvents(display, DefaultRootWindow(display), masks, 1);
	XFlush(display);

	// XFixes ask for cursor change events
	XFixesSelectCursorInput(display, root, XFixesDisplayCursorNotifyMask);

	// XDamage ask for damage events
//	XDamageCreate(display, root, XDamageReportDeltaRectangles);

	// XShm capture screenshot
	XShmSegmentInfo shminfo;

	XImage *image = XShmCreateImage(display, DefaultVisual(display, screen), DefaultDepth(display, screen), ZPixmap, NULL, &shminfo, capture_region.width, capture_region.height);

	shminfo.shmid = shmget(IPC_PRIVATE, image->bytes_per_line * image->height, IPC_CREAT | 0777);
	if(shminfo.shmid == -1) {
		// TODO handle error
		perror("shmget failed");
		return 1;
	}

	shminfo.shmaddr = image->data = shmat(shminfo.shmid, 0, 0);
	shminfo.readOnly = 0;

	if(!XShmAttach(display, &shminfo)) {
		fprintf(stderr, "Failed to attach to shm");
		return 1;
	}

//	int fifo = open("/tmp/fifo", O_WRONLY);

	bool cursor_inside;
	bool already_outside;
	bool already_inside;

	int x, y;
	int prevX, prevY;

	while(true) {
		XEvent event;
		XNextEvent(display, &event);

		if(event.type == GenericEvent) {
			XGetEventData(display, &event.xcookie);
			if (event.xcookie.evtype == XI_RawMotion) {
				XQueryPointer(display, root, (Window *) &unused, (Window *) &unused, &x, &y, (int *) &unused,
				              (int *) &unused, (unsigned *) &unused);
				if (prevX != x || prevY != y) {
					cursor_inside = inside(x, y, &capture_region);
					if(cursor_inside) {
						if(!already_inside) {
							report_cursor_img(display, sockfd);
							printf("Inside\n");
						}
						report_cursor_pos(sockfd, x - capture_region.x, y - capture_region.y);
//						printf("(%d, %d)\n", x - capture_region.x, y - capture_region.y);
						already_outside = false;
						already_inside = true;
					} else {
						if (!already_outside) {
							report_cursor_outside(sockfd);
							printf("Outside\n");
						}
						already_outside = true;
						already_inside = false;
					}
					prevX = x;
					prevY = y;
				}
			}
			XFreeEventData(display, &event.xcookie);
		} else if(event.type == xfixes_base_event_type + XFixesCursorNotify) {
			if(cursor_inside) {
				report_cursor_img(display, sockfd);
				printf("Cursor change!\n");
			}
		} else if(event.type == xdamage_base_event_type + XDamageNotify) {
			XDamageNotifyEvent *ev = (XDamageNotifyEvent *) &event;
			XRectangle area = ev->area;
			struct region r = {area.x, area.y, area.width, area.height};
			struct region clipped;
			if(clip(&r, &capture_region, &clipped)) {
//				schro_encoder_new
				printf("%d %d %d %d\n", clipped.x - capture_region.x, clipped.y - capture_region.y, clipped.width, clipped.height);
				XDamageSubtract(display, ev->damage, None, None);
				printf("Damage\n");
			}
		} else {
			printf("Unknown event: %d\n", event.type);
			return 0;
		}
//		if(!XShmGetImage(display, root, image, capture_region.x, capture_region.y, AllPlanes)) {
//			fprintf(stderr, "Failed to capture image");
//			return 1;
//		}
//
//		write(fifo, image->data, image->bytes_per_line * image->height / 5);
//
//		++frame_ctr;
	}

	return 0;
}
