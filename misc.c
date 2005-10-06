/* evilwm - Minimalist Window Manager for X
 * Copyright (C) 1999-2005 Ciaran Anscomb <evilwm@6809.org.uk>
 * see README for license and other details. */

#include <X11/Xproto.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "evilwm.h"
#include "log.h"

int need_client_tidy = 0;
int ignore_xerror = 0;

/* Now do this by fork()ing twice so we don't have to worry about SIGCHLDs */
void spawn(const char *const cmd[]) {
	pid_t pid;

	if (current_screen && current_screen->display)
		putenv(current_screen->display);
	if (!(pid = fork())) {
		setsid();
		switch (fork()) {
			/* Expect compiler warnings because of half-broken SUS
			 * execvp prototype:  "char *const argv[]" should have
			 * been "const char *const argv[]", but the committee
			 * favored legacy code over modern code, and modern
			 * compilers bark at our extra const. (LD) */
			case 0: execvp(cmd[0], cmd+1);
			default: _exit(0);
		}
	}
	if (pid > 0)
		wait(NULL);
}

void handle_signal(int signo) {
	(void)signo;  /* unused */
	int i;
	/* SIGCHLD check no longer necessary */
	/* Quit Nicely */
	while(head_client) remove_client(head_client);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	if (font) XFreeFont(dpy, font);
	for (i = 0; i < num_screens; i++) {
		XFreeGC(dpy, screens[i].invert_gc);
		XInstallColormap(dpy, DefaultColormap(dpy, screens[i].screen));
	}
	free(screens);
	XCloseDisplay(dpy);
	exit(0);
}

int handle_xerror(Display *dsply, XErrorEvent *e) {
	(void)dsply;  /* unused */
	Client *c;

	if (ignore_xerror) {
		LOG_DEBUG("handle_xerror() ignored an XErrorEvent: %d\n", e->error_code);
		return 0;
	}
	c = find_client(e->resourceid);
	/* If this error actually occurred while setting up the new
	 * window, best let make_new_client() know not to bother */
	if (initialising != None && e->resourceid == initialising) {
		LOG_DEBUG("\t **SAVED?** handle_xerror() caught error %d while initialising\n", e->error_code);
		initialising = None;
		return 0;
	}
	LOG_DEBUG("**ERK** handle_xerror() caught an XErrorEvent: error_code=%d request_code=%d minor_code=%d\n",
			e->error_code, e->request_code, e->minor_code);
	/* if (e->error_code == BadAccess && e->resourceid == root) { */
	if (e->error_code == BadAccess && e->request_code == X_ChangeWindowAttributes) {
		LOG_ERROR("root window unavailable (maybe another wm is running?)\n");
		exit(1);
	}

	/* Kludge around IE misbehaviour */
	if (e->error_code == 0x8 && e->request_code == 0x0c && e->minor_code == 0x00) {
		LOG_DEBUG("\thandle_xerror() : IE kludge - ignoring XError\n");
		return 0;
	}

	if (c) {
		LOG_DEBUG("\thandle_xerror() : flagging client for removal\n");
		c->remove = 1;
		need_client_tidy = 1;
	}
	return 0;
}

#ifdef DEBUG
void dump_clients() {
	Client *c;
	XWindowAttributes attr;

	for (c = head_client; c; c = c->next) {
		XGetWindowAttributes(dpy, c->window, &attr);
		LOG_DEBUG("MISC: (%d, %d) @ %d,%d\n", attr.map_state,
			c->ignore_unmap, c->x, c->y);
	}
}
#endif /* DEBUG */
