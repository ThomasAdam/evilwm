/* evilwm - Minimalist Window Manager for X
 * Copyright (C) 1999-2011 Ciaran Anscomb <evilwm@6809.org.uk>
 * see README for license and other details. */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "evilwm.h"
#include "log.h"

static void grab_keysym(Window w, unsigned int mask, KeySym keysym);
static void fix_screen_client(struct client * c, const struct physical_screen * old_phy);

static void
recalculate_sweep(struct client * c, int x1, int y1, int x2, int y2, unsigned force)
{
	struct geometry	*g, *gp;

	g = &c->current;
	gp = &c->prev;

	if (force || gp->w == 0) {
		gp->w = 0;
		g->w = abs(x1 - x2);
		g->w -= (g->w - c->hints.base_width) % c->hints.width_inc;
		if (c->hints.min_width && g->w < c->hints.min_width)
			g->w = c->hints.min_width;
		if (c->hints.max_width && g->w > c->hints.max_width)
			g->w = c->hints.max_width;
		g->x = (x1 <= x2) ? x1 : x1 - g->w;
	}
	if (force || gp->h == 0) {
		gp->h = 0;
		g->h = abs(y1 - y2);
		g->h -= (g->h - c->hints.base_height) % c->hints.height_inc;
		if (c->hints.min_height && g->h < c->hints.min_height)
			g->h = c->hints.min_height;
		if (c->hints.max_height && g->h > c->hints.max_height)
			g->h = c->hints.max_height;
		g->y = (y1 <= y2) ? y1 : y1 - g->h;
	}
}

void
sweep(struct client * c)
{
	struct geometry	*g = &c->current;
	XEvent		 ev;
	int		 old_cx = client_to_Xcoord(c, x);
	int		 old_cy = client_to_Xcoord(c, y);

	if (!grab_pointer(c->screen->root, MouseMask, resize_curs))
		return;

	client_raise(c);
	annotate_create(c, &annotate_sweep_ctx);

	setmouse(c->window, g->w, g->h);
	for (;;) {
		XMaskEvent(dpy, MouseMask, &ev);
		switch (ev.type) {
			case MotionNotify:
				if (ev.xmotion.root != c->screen->root)
					break;
				annotate_preupdate(c, &annotate_sweep_ctx);
				/* perform recalculate_sweep in Xcoordinates, then convert
				 * back relative to the current phy */
				recalculate_sweep(c, old_cx, old_cy,
					ev.xmotion.x, ev.xmotion.y,
					ev.xmotion.state & altmask);
				g->x -= c->phy->xoff;
				g->y -= c->phy->yoff;
				client_calc_cog(c);
				client_calc_phy(c);
				annotate_update(c, &annotate_sweep_ctx);
				break;
			case ButtonRelease:
				annotate_remove(c, &annotate_sweep_ctx);
				client_calc_phy(c);
				XUngrabPointer(dpy, CurrentTime);
				moveresizeraise(c);
				/* In case maximise state has changed: */
				ewmh_set_net_wm_state(c);
				return;
			default:
				break;
		}
	}
}

/** predicate_keyrepeatpress:
 *  predicate function for use with XCheckIfEvent.
 *  When used with XCheckIfEvent, this function will return true if
 *  there is a KeyPress event queued of the same keycode and time
 *  as @arg.
 *
 *  @arg must be a poiner to an XEvent of type KeyRelease
 */
static Bool
predicate_keyrepeatpress(Display * dummy, XEvent * ev, XPointer arg)
{
	(void) dummy;
	XEvent     *release_event = (XEvent *) arg;

	if (ev->type != KeyPress)
		return False;
	if (release_event->xkey.keycode != ev->xkey.keycode)
		return False;
	return release_event->xkey.time == ev->xkey.time;
}

void
show_info(struct client * c, unsigned int keycode)
{
	XEvent      ev;
	XKeyboardState keyboard;

	if (XGrabKeyboard(dpy, c->screen->root, False, GrabModeAsync,
			GrabModeAsync, CurrentTime) != GrabSuccess)
		return;

	/* keyboard repeat might not have any effect, newer X servers seem to
	 * only change the keyboard control after all keys have been physically
	 * released. */
	XGetKeyboardControl(dpy, &keyboard);
	XChangeKeyboardControl(dpy, KBAutoRepeatMode, &(XKeyboardControl) {
		.auto_repeat_mode = AutoRepeatModeOff}
	);
	annotate_create(c, &annotate_info_ctx);
	do {
		XMaskEvent(dpy, KeyReleaseMask, &ev);
		if (ev.xkey.keycode != keycode)
			continue;
		if (XCheckIfEvent(dpy, &ev, predicate_keyrepeatpress,
				(XPointer) & ev)) {
			/* This is a key press event with the same time as the previous
			 * key release event. */
			continue;
		}
		break;		/* escape */
	} while (1);
	annotate_remove(c, &annotate_info_ctx);
	XChangeKeyboardControl(dpy, KBAutoRepeatMode, &(XKeyboardControl) {
		.auto_repeat_mode = keyboard.global_auto_repeat}
	);
	XUngrabKeyboard(dpy, CurrentTime);
}

static int
absmin(int a, int b)
{
	if (abs(a) < abs(b))
		return a;
	return b;
}

static void
snap_client(struct client * c)
{
	int		 dx, dy;
	struct list	*iter;
	struct client	*ci;
	struct geometry	*g, *g_ci;
	int		 c_screen_x = client_to_Xcoord(c, x);
	int		 c_screen_y = client_to_Xcoord(c, y);
	int		 ci_screen_x, ci_screen_y;

	g = &c->current;

	/* snap to other windows */
	dx = dy = opt_snap;
	for (iter = clients_tab_order; iter; iter = iter->next) {
		ci = iter->data;
		ci_screen_x = client_to_Xcoord(ci, x);
		ci_screen_y = client_to_Xcoord(ci, y);

		if (ci == c)
			continue;
		if (ci->screen != c->screen)
			continue;
		if (!is_fixed(ci) && ci->vdesk != c->phy->vdesk)
			continue;
		if (ci->is_dock && !c->screen->docks_visible)
			continue;

		g_ci = &ci->current;

		if (ci_screen_y - g_ci->border_width - g->border_width - g->h -
			c_screen_y <= opt_snap
			&& c_screen_y - g->border_width - g_ci->border_width - g_ci->h -
			ci_screen_y <= opt_snap) {
			dx = absmin(dx,
				ci_screen_x + g_ci->w - c_screen_x +
				g->border_width + g_ci->border_width);
			dx = absmin(dx,
				ci_screen_x + g_ci->w - c_screen_x -
				g->w);
			dx = absmin(dx,
				ci_screen_x - c_screen_x - g->w -
				g->border_width - g_ci->border_width);
			dx = absmin(dx, ci_screen_x - c_screen_x);
		}
		if (ci_screen_x - g_ci->border_width - g->border_width - g->w -
			c_screen_x <= opt_snap
			&& c_screen_x - g->border_width - g_ci->border_width - g_ci->w -
			ci_screen_x <= opt_snap) {
			dy = absmin(dy,
				ci_screen_y + g_ci->h - c_screen_y +
				g->border_width + g_ci->border_width);
			dy = absmin(dy,
				ci_screen_y + g_ci->h - c_screen_y -
				g->h);
			dy = absmin(dy,
				ci_screen_y - c_screen_y - g->h -
				g->border_width - g_ci->border_width);
			dy = absmin(dy, ci_screen_y - c_screen_y);
		}
	}
	if (abs(dx) < opt_snap)
		g->x += dx;
	if (abs(dy) < opt_snap)
		g->y += dy;

	/* snap to screen border */
	if (abs(g->x - g->border_width) < opt_snap)
		g->x = g->border_width;
	if (abs(g->y - g->border_width) < opt_snap)
		g->y = g->border_width;
	if (abs(g->x + g->w + g->border_width - c->phy->width) < opt_snap)
		g->x = c->phy->width - g->w - g->border_width;
	if (abs(g->y + g->h + g->border_width - c->phy->height) < opt_snap)
		g->y = c->phy->height - g->h - g->border_width;

	if (abs(g->x) == g->border_width && g->w == c->phy->width)
		g->x = 0;
	if (abs(g->y) == g->border_width && g->h == c->phy->height)
		g->y = 0;
}

void
drag(struct client * c)
{
	struct geometry	 g = c->current;
	XEvent		 ev;
	int		 x1, y1;	/* pointer position at start of grab in screen co-ordinates */
	int		 screen_x, screen_y;
	int		 old_screen_x = client_to_Xcoord(c, x);
	int		 old_screen_y = client_to_Xcoord(c, y);;

	if (!grab_pointer(c->screen->root, MouseMask, move_curs))
		return;

	client_raise(c);
	get_mouse_position(&x1, &y1, c->screen->root);
	annotate_create(c, &annotate_drag_ctx);
	for (;;) {
		XMaskEvent(dpy, MouseMask, &ev);
		switch (ev.type) {
			case MotionNotify:
				if (ev.xmotion.root != c->screen->root)
					break;
				annotate_preupdate(c, &annotate_drag_ctx);
				screen_x = old_screen_x + (ev.xmotion.x - x1);
				screen_y = old_screen_y + (ev.xmotion.y - y1);
				client_update_screenpos(c, screen_x, screen_y);
				client_calc_phy(c);
				if (opt_snap && !(ev.xmotion.state & altmask))
					snap_client(c);

				if (!no_solid_drag) {
					XMoveWindow(dpy, c->parent,
						client_to_Xcoord(c, x) -
						    g.border_width,
						client_to_Xcoord(c,y) -
						    g.border_width);
					send_config(c);
				}
				annotate_update(c, &annotate_drag_ctx);
				break;
			case ButtonRelease:
				annotate_remove(c, &annotate_drag_ctx);
				XUngrabPointer(dpy, CurrentTime);
				if (no_solid_drag) {
					moveresizeraise(c);
				}
				return;
			default:
				break;
		}
	}
}

/* limit the client to a visible position on the current phy */
void
position_policy(struct client * c)
{
	struct geometry	*g = &c->current;
	g->x = MAX(1 - g->w - g->border_width, MIN(g->x, c->phy->width));
	g->y = MAX(1 - g->h - g->border_width, MIN(g->y, c->phy->height));
}

void
moveresize(struct client * c)
{
	struct geometry	 g = c->current;

	position_policy(c);
	XMoveResizeWindow(dpy, c->parent,
		client_to_Xcoord(c, x) - g.border_width,
		client_to_Xcoord(c,y) - g.border_width, g.w, g.h);
	XMoveResizeWindow(dpy, c->window, 0, 0, g.w, g.h);
	send_config(c);
}

void
moveresizeraise(struct client * c)
{
	client_raise(c);
	moveresize(c);
}

void
maximise_client(struct client * c, int action, int hv)
{
	struct geometry	*g = &c->current;
	struct geometry *gp = &c->prev;

	if (hv & MAXIMISE_FULLSCREEN)
		hv |= MAXIMISE_HORZ|MAXIMISE_VERT;

	if (hv & MAXIMISE_HORZ) {
		if (gp->w) {
			if (action == NET_WM_STATE_REMOVE
				|| action == NET_WM_STATE_TOGGLE) {
				g->x = gp->x;
				g->w = gp->w;
				gp->w = 0;
				XDeleteProperty(dpy, c->window,
					xa_evilwm_unmaximised_horz);
			}
		} else {
			if (action == NET_WM_STATE_ADD
				|| action == NET_WM_STATE_TOGGLE) {
				unsigned long props[2];

				gp->x = g->x;
				gp->w = g->w;
				g->x = 0 + g->border_width;
				g->w = c->phy->width - g->border_width * 2;
				props[0] = gp->x;
				props[1] = gp->w;
				XChangeProperty(dpy, c->window,
					xa_evilwm_unmaximised_horz,
					XA_CARDINAL, 32, PropModeReplace,
					(unsigned char *) &props, 2);
			}
		}
	}
	if (hv & MAXIMISE_VERT) {
		if (g->h) {
			if (action == NET_WM_STATE_REMOVE
				|| action == NET_WM_STATE_TOGGLE) {
				g->y = gp->y;
				g->h = gp->h;
				gp->h = 0;
				XDeleteProperty(dpy, c->window,
					xa_evilwm_unmaximised_vert);
			}
		} else {
			if (action == NET_WM_STATE_ADD
				|| action == NET_WM_STATE_TOGGLE) {
				unsigned long props[2];

				gp->y = g->y;
				gp->h = g->h;
				g->y = 0 + g->border_width;
				g->h = c->phy->height - g->border_width * 2;
				props[0] = gp->y;
				props[1] = gp->h;
				XChangeProperty(dpy, c->window,
					xa_evilwm_unmaximised_vert,
					XA_CARDINAL, 32, PropModeReplace,
					(unsigned char *) &props, 2);
			}
		}
	}
	/* If we're toggling fullscreen, where fullscreen is defined as both
	 * vert/horiz set, then remove window borders, and put them back
	 * again.
	 */
	if (hv & MAXIMISE_FULLSCREEN) {
		if (action == NET_WM_STATE_TOGGLE ||
			action == NET_WM_STATE_ADD) {
			XWindowAttributes attr;

			XGetWindowAttributes(dpy, c->parent, &attr);
			if (attr.border_width != 0) {
				gp->border_width = g->border_width;
				g->border_width = 0;
				g->x = g->y = 0;
				g->w = c->phy->width;
				g->h = c->phy->height;
				XSetWindowBorderWidth(dpy, c->parent, 0);
			} else {

				XSetWindowBorderWidth(dpy, c->parent,
					gp->border_width);
				g->border_width = gp->border_width;
			}
		} else {
			XSetWindowBorderWidth(dpy, c->parent, gp->border_width);
			g->border_width = gp->border_width;
		}

	}

	/* xinerama: update the client's centre of gravity
	 *  NB, the client doesn't change physical screen */
	client_calc_cog(c);
	ewmh_set_net_wm_state(c);
	moveresizeraise(c);
	discard_enter_events(c);
}

void
next(void)
{
	struct list *newl = list_find(clients_tab_order, current);
	struct client     *newc = current;

	do {
		if (newl) {
			newl = newl->next;
			if (!newl && !current)
				return;
		}
		if (!newl)
			newl = clients_tab_order;
		if (!newl)
			return;
		newc = newl->data;
		if (newc == current)
			return;
	}
	/* NOTE: Checking against newc->screen->vdesk implies we can Alt+Tab
	 * across screen boundaries.  Is this what we want? */
	while ((!is_fixed(newc) && (newc->vdesk != newc->phy->vdesk))
		|| newc->is_dock);

	if (!newc)
		return;
	client_show(newc);
	client_raise(newc);
	select_client(newc);
#ifdef WARP_POINTER
	setmouse(newc->window, newc->current.w + newc->current.border_width - 1,
		newc->current.h + newc->current.border_width - 1);
#endif
	discard_enter_events(newc);
}

/** switch_vdesk:
 *  Switch the virtual desktop on physical screen @p of logical screen @s
 *  to @v
 */
bool
switch_vdesk(struct screen_info * s, struct physical_screen * p, unsigned int v)
{
	struct list *iter;

#ifdef DEBUG
	int         hidden = 0, raised = 0;
#endif
	if (!valid_vdesk(v) && v != VDESK_NONE)
		return false;

	/* no-op if a physical screen is already displaying @v */
	for (unsigned i = 0; i < (unsigned) s->num_physical; i++) {
		if (v == s->physical[i].vdesk)
			return false;
	}

	LOG_ENTER("switch_vdesk(screen=%d, from=%u, to=%u)", s->screen,
		p->vdesk, v);
	if (current && !is_fixed(current)) {
		select_client(NULL);
	}
	for (iter = clients_tab_order; iter; iter = iter->next) {
		struct client     *c = iter->data;

		if (c->screen != s)
			continue;
		if (c->vdesk == p->vdesk) {
			client_hide(c);
#ifdef DEBUG
			hidden++;
#endif
		} else if (c->vdesk == v) {
			/* NB, vdesk may not be on the same physical screen as previously,
			 * so move windows onto the physical screen */
			if (c->phy != p) {
				struct physical_screen *old_phy = c->phy;

				c->phy = p;
				fix_screen_client(c, old_phy);
			}
			if (!c->is_dock || s->docks_visible)
				client_show(c);
#ifdef DEBUG
			raised++;
#endif
		}
	}
	/* cache the value of the current vdesk, so that user may toggle back to it */
	s->old_vdesk = p->vdesk;
	p->vdesk = v;
	ewmh_set_net_current_desktop(s);
	LOG_DEBUG("%d hidden, %d raised\n", hidden, raised);
	LOG_LEAVE();

	return true;
}

void
exchange_phy(struct screen_info * s)
{
	if (s->num_physical != 2) {
		/* todo: handle multiple phys */
		return;
	}
	/* this action should not alter the old_vdesk, so user may still toggle to it */
	unsigned    save_old_vdesk = s->old_vdesk;
	unsigned    vdesk_a = s->physical[0].vdesk;
	unsigned    vdesk_b = s->physical[1].vdesk;

	/* clear the vdesks to stop switch_vdesk discovering vdesk is
	 * already mapped and ignoring the request */
	s->physical[0].vdesk = s->physical[1].vdesk = VDESK_NONE;
	switch_vdesk(s, &s->physical[0], vdesk_b);
	switch_vdesk(s, &s->physical[1], vdesk_a);
	s->old_vdesk = save_old_vdesk;
}

void
set_docks_visible(struct screen_info * s, int is_visible)
{
	struct list *iter;

	LOG_ENTER("set_docks_visible(screen=%d, is_visible=%d)", s->screen,
		is_visible);
	s->docks_visible = is_visible;
	for (iter = clients_tab_order; iter; iter = iter->next) {
		struct client     *c = iter->data;

		if (c->screen != s)
			continue;
		if (c->is_dock) {
			if (is_visible) {
				if (is_fixed(c) || (c->vdesk == c->phy->vdesk)) {
					client_show(c);
					client_raise(c);
				}
			} else {
				client_hide(c);
			}
		}
	}
	LOG_LEAVE();
}

static int
scale_pos(int new_screen_size, int old_screen_size, int cli_pos, int cli_size,
	int border)
{
	cli_size += 2 * border;
	new_screen_size -= cli_size;
	old_screen_size -= cli_size;
	if (old_screen_size <= 0 || new_screen_size <= 0)
		return cli_pos;
	return new_screen_size * (cli_pos - border) / old_screen_size + border;
}

static void
fix_screen_client(struct client * c, const struct physical_screen * old_phy)
{
	struct geometry	*g, *gp;
	int		 oldw = old_phy->width;
	int		 oldh = old_phy->height;
	int		 neww = c->phy->width;
	int		 newh = c->phy->height;

	g = &c->current;
	gp = &c->prev;

	if (gp->w) {
		/* horiz maximised: update width, update old x pos */
		g->w = neww;
		gp->x = scale_pos(neww, oldw, gp->x, gp->w, g->border_width);
	} else {
		/* horiz normal: update x pos */
		g->x = scale_pos(neww, oldw, g->x, g->w, g->border_width);
	}

	if (gp->h) {
		/* vert maximised: update height, update old y pos */
		g->h = newh;
		gp->y = scale_pos(newh, oldh, gp->y, gp->h, g->border_width);
	} else {
		/* vert normal: update y pos */
		g->y = scale_pos(newh, oldh, g->y, g->h, g->border_width);
	}

	client_calc_cog(c);
	moveresize(c);
}

struct screen_info *
find_screen(Window root)
{
	int         i;

	for (i = 0; i < num_screens; i++) {
		if (screens[i].root == root)
			return &screens[i];
	}
	return NULL;
}

struct screen_info *
find_current_screen(void)
{
	struct screen_info *current_screen;

	find_current_screen_and_phy(&current_screen, NULL);
	return current_screen;
}

void
find_current_screen_and_phy(struct screen_info ** current_screen,
	struct physical_screen ** current_phy)
{
	Window      cur_root, dw;
	int         di;
	unsigned int dui;
	int         x, y;

	/* XQueryPointer is useful for getting the current pointer root */
	XQueryPointer(dpy, screens[0].root, &cur_root, &dw, &x, &y, &di, &di,
		&dui);
	*current_screen = find_screen(cur_root);
	if (current_phy)
		*current_phy = find_physical_screen(*current_screen, x, y);
}

/** find_physical_screen:
 *   Given a logical screen, find which physical screen the point
 *   (@screen_x,@screen_y) resides.
 *
 *   If the point isn't on a physical screen, finds the closest screen
 *   centre.
 */
struct physical_screen *
find_physical_screen(struct screen_info * screen, int screen_x, int screen_y)
{
	struct physical_screen *phy = NULL;

	/* Find if (screen_x,y) is on any physical screen */
	for (unsigned i = 0; i < (unsigned) screen->num_physical; i++) {
		phy = &screen->physical[i];
		if (screen_x >= phy->xoff && screen_x <= phy->xoff + phy->width
			&& screen_y >= phy->yoff
			&& screen_y <= phy->yoff + phy->height)
			return phy;
	}

	/* fall back to finding the closest screen minimum distance between the
	 * physical screen centre to (screen_x,y) */
	int         val = INT_MAX;

	for (unsigned i = 0; i < (unsigned) screen->num_physical; i++) {
		struct physical_screen *p = &screen->physical[i];

		int         dx = screen_x - p->xoff - p->width / 2;
		int         dy = screen_y - p->yoff - p->height / 2;

		if (dx * dx + dy * dy < val) {
			val = dx * dx + dy * dy;
			phy = p;
		}
	}
	return phy;
}

static void
grab_keysym(Window w, unsigned int mask, KeySym keysym)
{
	KeyCode     keycode = XKeysymToKeycode(dpy, keysym);

	XGrabKey(dpy, keycode, mask, w, True, GrabModeAsync, GrabModeAsync);
	XGrabKey(dpy, keycode, mask | LockMask, w, True,
		GrabModeAsync, GrabModeAsync);
	if (numlockmask) {
		XGrabKey(dpy, keycode, mask | numlockmask, w, True,
			GrabModeAsync, GrabModeAsync);
		XGrabKey(dpy, keycode, mask | numlockmask | LockMask, w, True,
			GrabModeAsync, GrabModeAsync);
	}
}

void
grab_keys_for_screen(struct screen_info * s)
{
	const KeySym keys_to_grab[] = {
#ifdef VWM
		KEY_FIX, KEY_PREVDESK, KEY_NEXTDESK, KEY_TOGGLEDESK,
		XK_1, XK_2, XK_3, XK_4, XK_5, XK_6, XK_7, XK_8, XK_9, XK_0,
		KEY_EXGPHY,
#endif
		KEY_NEW, KEY_KILL,
		KEY_TOPLEFT, KEY_TOPRIGHT, KEY_BOTTOMLEFT, KEY_BOTTOMRIGHT,
		KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_UP,
		KEY_LOWER, KEY_ALTLOWER, KEY_INFO, KEY_MAXVERT, KEY_MAX,
		KEY_FULLSCREEN, KEY_DOCK_TOGGLE
	};
#define NUM_GRABS (int)(sizeof(keys_to_grab) / sizeof(KeySym))

	const KeySym alt_keys_to_grab[] = {
		KEY_KILL, KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_UP,
		KEY_MAXVERT,
	};
#define NUM_ALT_GRABS (int)(sizeof(alt_keys_to_grab) / sizeof(KeySym))

	int         i;

	/* Release any previous grabs */
	XUngrabKey(dpy, AnyKey, AnyModifier, s->root);
	/* Grab key combinations we're interested in */
	for (i = 0; i < NUM_GRABS; i++) {
		grab_keysym(s->root, grabmask1, keys_to_grab[i]);
	}
	for (i = 0; i < NUM_ALT_GRABS; i++) {
		grab_keysym(s->root, grabmask1 | altmask, alt_keys_to_grab[i]);
	}
	grab_keysym(s->root, grabmask2, KEY_NEXT);
}

/*
 * physical screen discovery methods
 */

#ifdef XINERAMA
static bool
probe_screen_xinerama(void)
{
	if (!have_xinerama) {
		return false;
	}

	/* xinerama as exposed by xlib does not allow multiple screens per display */
	if (num_screens > 1) {
		LOG_ERROR
			("Interesting, xinerama present, but there are multiple screens\n");
		return false;
	}

	int         num_phy_screens;
	XineramaScreenInfo *xin_scr_info =
		XineramaQueryScreens(dpy, &num_phy_screens);
	if (!num_phy_screens) {
		return false;
	}

	struct physical_screen *new_phys =
		malloc(num_phy_screens * sizeof(struct physical_screen));
	for (int j = 0; j < num_phy_screens; j++) {
		new_phys[j].xoff = xin_scr_info[j].x_org;
		new_phys[j].yoff = xin_scr_info[j].y_org;
		new_phys[j].width = xin_scr_info[j].width;
		new_phys[j].height = xin_scr_info[j].height;
	}
	if (xin_scr_info)
		XFree(xin_scr_info);

	screens[0].physical = new_phys;
	screens[0].num_physical = num_phy_screens;
	return true;
}
#endif

static void
probe_screen_default(struct screen_info * s)
{
	s->num_physical = 1;
	s->physical = malloc(sizeof(struct physical_screen));
	s->physical->xoff = 0;
	s->physical->yoff = 0;
	s->physical->width = DisplayWidth(dpy, s->screen);
	s->physical->height = DisplayHeight(dpy, s->screen);
}

#ifdef RANDR
static bool
probe_screen_xrandr(struct screen_info * s)
{
	if (!have_randr)
		return false;

	int         xrandr_major, xrandr_minor;

	XRRQueryVersion(dpy, &xrandr_major, &xrandr_minor);
	if (xrandr_major == 1 && xrandr_minor < 2)
		return false;

	LOG_ENTER("probe_screen(screen=%d)", s->screen);

#if (RANDR_MAJOR > 1 || RANDR_MAJOR == 1 && RANDR_MINOR >= 3)
	XRRScreenResources *rr_screenres =
		XRRGetScreenResourcesCurrent(dpy, s->root);
#else
	XRRScreenResources *rr_screenres = XRRGetScreenResources(dpy, s->root);
#endif

	/* assume a single crtc per physical screen, clean up later */
	int         num_physical = rr_screenres->ncrtc;
	struct physical_screen *new_phys =
		malloc(num_physical * sizeof(struct physical_screen));

	if (num_physical == 0) {
		LOG_LEAVE();
		return false;
	}

	for (int j = 0; j < num_physical; j++) {
		XRRCrtcInfo *rr_crtc =
			XRRGetCrtcInfo(dpy, rr_screenres,
			rr_screenres->crtcs[j]);
		new_phys[j].xoff = rr_crtc->x;
		new_phys[j].yoff = rr_crtc->y;
		new_phys[j].width = rr_crtc->width;
		new_phys[j].height = rr_crtc->height;
		LOG_DEBUG
			("discovered: phy[%d]{.xoff=%d, .yoff=%d, .width=%d, .height=%d}\n",
			j, rr_crtc->x, rr_crtc->y, rr_crtc->width,
			rr_crtc->height);
		XRRFreeCrtcInfo(rr_crtc);
	}

	/* prune all duplicates ⊂ new_phys */
	for (int j = 0; j < num_physical; j++) {
		for (int k = 0; k < num_physical; k++) {
			if (k == j)
				continue;
			if (new_phys[k].xoff < new_phys[j].xoff)
				continue;
			if (new_phys[k].yoff < new_phys[j].yoff)
				continue;
			if (new_phys[k].xoff + new_phys[k].width >
				new_phys[j].xoff + new_phys[j].width)
				continue;
			if (new_phys[k].yoff + new_phys[k].height >
				new_phys[j].yoff + new_phys[j].height)
				continue;
			/* k ⊂ j: delete k */
			LOG_DEBUG("pruning %d\n", k);
			memmove(&new_phys[k], &new_phys[k + 1],
				(num_physical - k - 1) * sizeof(*new_phys));
			num_physical--;
			if (j > k)
				j--;
			k--;
		}
	}
	s->physical = new_phys;
	s->num_physical = num_physical;

	XRRFreeScreenResources(rr_screenres);

	LOG_LEAVE();
	return true;
}
#endif

void
probe_screen(struct screen_info * s)
{
#ifdef RANDR
	if (probe_screen_xrandr(s))
		return;
#endif

#ifdef XINERAMA
	if (probe_screen_xinerama())
		return;
#endif

	probe_screen_default(s);
}
