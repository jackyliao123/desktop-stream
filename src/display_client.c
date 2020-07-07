#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <X11/Xlib.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/Xatom.h>
#include <GL/glx.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

int new_socket(char *listen_host, char *listen_port, char *connect_host, char *connect_port) {
	struct addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_flags = AI_PASSIVE;

	struct addrinfo *res = NULL;

//	if((s = getaddrinfo(listen_host, listen_port, &hints, &res)) != 0) {
//		fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(s));
//		return -1;
//	}

	int s;
	if((s = getaddrinfo(connect_host, connect_port, &hints, &res)) != 0) {
		fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(s));
		return -1;
	}

	int fd;
	if((fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) == -1) {
		perror("socket failed");
		goto err;
	}

//	if(bind(fd, res->ai_addr, res->ai_addrlen) == -1) {
//		perror("bind failed");
//		goto err;
//	}
//	freeaddrinfo(res);
//	res = NULL;

	if(connect(fd, res->ai_addr, res->ai_addrlen) == -1) {
		perror("connect failed");
		goto err;
	}

	send(fd, NULL, 0, 0);

	return fd;

err:
	close(fd);
	if(res) {
		freeaddrinfo(res);
	}

	return -1;
}

uint8_t cursor_buf[262144];
size_t cursor_len;

void update_cursor(Display *dpy, Window win, int x, int y, int width, int height) {
	XcursorImage *image = XcursorImageCreate(width, height);
	image->xhot = x;
	image->yhot = y;
	image->delay = 0;
	memcpy(image->pixels, cursor_buf, cursor_len);

	Cursor cursor = XcursorImageLoadCursor(dpy, image);
	XcursorImageDestroy(image);

	XDefineCursor(dpy, win, cursor);

	XFreeCursor(dpy, cursor);
	XFlush(dpy);
}

void remove_cursor(Display *dpy, Window win) {
	cursor_len = 4;
	memset(cursor_buf, 0, cursor_len);
	update_cursor(dpy, win, 0, 0, 1, 1);
}

int main() {
	int sockfd = new_socket("127.0.0.1", NULL, "10.42.0.1", "9999");
	if(sockfd == -1) {
		fprintf(stderr, "failed to connect\n");
		return 1;
	}
//	int fd = 0;

	Display                 *dpy;
	Window                  root;
	GLint                   att[] = { GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None };
	XVisualInfo             *vi;
	Colormap                cmap;
	XSetWindowAttributes    swa;
	Window                  win;
	GLXContext              glc;
	XWindowAttributes       gwa;
	XEvent                  xev;

	dpy = XOpenDisplay(NULL);

	if(dpy == NULL) {
		printf("\n\tcannot connect to X server\n\n");
		exit(0);
	}

	root = DefaultRootWindow(dpy);

	vi = glXChooseVisual(dpy, 0, att);

	if(vi == NULL) {
		printf("\n\tno appropriate visual found\n\n");
		exit(0);
	}
	else {
		printf("\n\tvisual %p selected\n", (void *)vi->visualid); /* %p creates hexadecimal output like in glxinfo */
	}


	cmap = XCreateColormap(dpy, root, vi->visual, AllocNone);

	swa.colormap = cmap;
	swa.event_mask = ExposureMask | KeyPressMask;

	win = XCreateWindow(dpy, root, 0, 0, 600, 600, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);

	Atom wm_state   = XInternAtom (dpy, "_NET_WM_STATE", 1);
	Atom wm_fullscreen = XInternAtom (dpy, "_NET_WM_STATE_FULLSCREEN", 1);

	XChangeProperty(dpy, win, wm_state, XA_ATOM, 32,
	                PropModeReplace, (unsigned char *)&wm_fullscreen, 1);

	printf("%lu\n", win);


	XMapWindow(dpy, win);
	XStoreName(dpy, win, "VERY SIMPLE APPLICATION");

	glc = glXCreateContext(dpy, vi, NULL, GL_TRUE);
	glXMakeCurrent(dpy, win, glc);

	glDrawBuffer(GL_FRONT);

//	char *buffer = malloc(1920 * 1080 * 4);

	char buf[4096];

	int x, y, offset;

	while(1) {
		int nread = recv(sockfd, buf, 4096, 0);
		if(nread == 0) {
			continue;
		}
		switch(buf[0]) {
			case 1:
				x = *(uint16_t *) (buf + 1);
				y = *(uint16_t *) (buf + 3);
				XWarpPointer(dpy, None, root, 0, 0, 0, 0, x, y);
				XFlush(dpy);
				break;
			case 2:
				printf("cursor data\n");
				offset = *(uint32_t *) (buf + 1);
				memcpy(cursor_buf + offset, buf + 5, nread - 5);
				cursor_len = MAX(cursor_len, offset + nread - 5);
				break;
			case 3:
				update_cursor(dpy, win, *(uint16_t *) (buf + 1), *(uint16_t *) (buf + 3), *(uint16_t *) (buf + 5), *(uint16_t *) (buf + 7));
				cursor_len = 0;
				printf("cursor change\n");
				break;
			case 4:
				remove_cursor(dpy, win);
				printf("cursor outside\n");
				break;
			default:
				printf("Invalid operation\n");
				return 1;
		}
//		int tr = 0;
//		int r = 0;
//		while ((r = read(fd, buffer + tr, 1920 * 1080 * 4 / 5 - tr)) > 0) {
//			tr += r;
//		}

//		XGetWindowAttributes(dpy, win, &gwa);
//		glViewport(0, 0, gwa.width, gwa.height);
//		glRasterPos2f(-1,1);
//		glPixelZoom( 1, -1 );
//		glDrawPixels(1920, 216, GL_BGRA, GL_UNSIGNED_BYTE, buffer);
//		glFlush();
//		glXSwapBuffers(dpy, win);

//		else if(xev.type == KeyPress) {
//			glXMakeCurrent(dpy, None, NULL);
//			glXDestroyContext(dpy, glc);
//			XDestroyWindow(dpy, win);
//			XCloseDisplay(dpy);
//			exit(0);
//		}
	} /* this closes while(1) { */


	return 0;
}
