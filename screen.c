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
	if (force || c->oldw == 0) {
		c->oldw = 0;
		c->width = abs(x1 - x2);
		c->width -= (c->width - c->base_width) % c->width_inc;
		if (c->min_width && c->width < c->min_width)
			c->width = c->min_width;
		if (c->max_width && c->width > c->max_width)
			c->width = c->max_width;
		c->nx = (x1 <= x2) ? x1 : x1 - c->width;
	}
	if (force || c->oldh == 0) {
		c->oldh = 0;
		c->height = abs(y1 - y2);
		c->height -= (c->height - c->base_height) % c->height_inc;
		if (c->min_height && c->height < c->min_height)
			c->height = c->min_height;
		if (c->max_height && c->height > c->max_height)
			c->height = c->max_height;
		c->ny = (y1 <= y2) ? y1 : y1 - c->height;
	}
}

void
sweep(struct client * c)
{
	XEvent      ev;
	int         old_cx = client_to_Xcoord(c, x);
	int         old_cy = client_to_Xcoord(c, y);

	if (!grab_pointer(c->screen->root, MouseMask, resize_curs))
		return;

	client_raise(c);
	annotate_create(c, &annotate_sweep_ctx);

	setmouse(c->window, c->width, c->height);
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
				c->nx -= c->phy->xoff;
				c->ny -= c->phy->yoff;
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
	int         dx, dy;
	struct list *iter;
	struct client     *ci;

	/* client in screen co-ordinates */
	int         c_screen_x = client_to_Xcoord(c, x);
	int         c_screen_y = client_to_Xcoord(c, y);

	/* snap to other windows */
	dx = dy = opt_snap;
	for (iter = clients_tab_order; iter; iter = iter->next) {
		ci = iter->data;
		int         ci_screen_x = client_to_Xcoord(ci, x);
		int         ci_screen_y = client_to_Xcoord(ci, y);

		if (ci == c)
			continue;
		if (ci->screen != c->screen)
			continue;
		if (!is_fixed(ci) && ci->vdesk != c->phy->vdesk)
			continue;
		if (ci->is_dock && !c->screen->docks_visible)
			continue;
		if (ci_screen_y - ci->border - c->border - c->height -
			c_screen_y <= opt_snap
			&& c_screen_y - c->border - ci->border - ci->height -
			ci_screen_y <= opt_snap) {
			dx = absmin(dx,
				ci_screen_x + ci->width - c_screen_x +
				c->border + ci->border);
			dx = absmin(dx,
				ci_screen_x + ci->width - c_screen_x -
				c->width);
			dx = absmin(dx,
				ci_screen_x - c_screen_x - c->width -
				c->border - ci->border);
			dx = absmin(dx, ci_screen_x - c_screen_x);
		}
		if (ci_screen_x - ci->border - c->border - c->width -
			c_screen_x <= opt_snap
			&& c_screen_x - c->border - ci->border - ci->width -
			ci_screen_x <= opt_snap) {
			dy = absmin(dy,
				ci_screen_y + ci->height - c_screen_y +
				c->border + ci->border);
			dy = absmin(dy,
				ci_screen_y + ci->height - c_screen_y -
				c->height);
			dy = absmin(dy,
				ci_screen_y - c_screen_y - c->height -
				c->border - ci->border);
			dy = absmin(dy, ci_screen_y - c_screen_y);
		}
	}
	if (abs(dx) < opt_snap)
		c->nx += dx;
	if (abs(dy) < opt_snap)
		c->ny += dy;

	/* snap to screen border */
	if (abs(c->nx - c->border) < opt_snap)
		c->nx = c->border;
	if (abs(c->ny - c->border) < opt_snap)
		c->ny = c->border;
	if (abs(c->nx + c->width + c->border - c->phy->width) < opt_snap)
		c->nx = c->phy->width - c->width - c->border;
	if (abs(c->ny + c->height + c->border - c->phy->height) < opt_snap)
		c->ny = c->phy->height - c->height - c->border;

	if (abs(c->nx) == c->border && c->width == c->phy->width)
		c->nx = 0;
	if (abs(c->ny) == c->border && c->height == c->phy->height)
		c->ny = 0;
}

void
client_expand(struct client *c)
{
	int		 c_screen_x = client_to_Xcoord(c, x);
	int		 c_screen_y = client_to_Xcoord(c, y);
	int		 i, n, relevant, x, y, w, h;
	int		 mx, my, mh, mw;
	int		 x1, y1, w1, h1;
	struct list	*iter;
	struct client	*ci;
	workarea	*regions;

	mx = my = mh = mw = 0;
	x1 = y1 = w1 = h1 = 0;

	fprintf(stderr, "Original client: %dx%d, pos: %dx%d, w: %d, h: %d\n",
		c->nx, c->ny, c_screen_x, c_screen_y, c->width, c->height);

	relevant = 0;
	for (iter = clients_tab_order; iter != NULL; iter = iter->next) {
		ci = iter->data;
		if (ci == c)
			continue;
		if (ci->screen != c->screen)
			continue;
		if (!is_fixed(ci) && ci->vdesk != c->phy->vdesk)
			continue;
		if (ci->is_dock && !c->screen->docks_visible)
			continue;

		relevant++;
	}

	// list of coords/sizes for fully visible windows on this desktop
	regions = malloc(sizeof(workarea) * relevant);
	if (regions == NULL) {
		fprintf(stderr, "Couldn't malloc!\n");
		return;
	}

	n = 0;
	for (iter = clients_tab_order; iter; iter = iter->next) {
		ci = iter->data;

		if (ci == c)
			continue;
		if (ci->screen != c->screen)
			continue;
		if (!is_fixed(ci) && ci->vdesk != c->phy->vdesk)
			continue;
		if (ci->is_dock && !c->screen->docks_visible)
			continue;
		if ((mw || mh)
		    && !INTERSECT(ci->nx, ci->ny, ci->width, ci->height,
			    mx, my, mw, mh))
			continue;

		regions[n].x = ci->nx;
		regions[n].y = ci->ny;
		regions[n].w = ci->width;
		regions[n].h = ci->height;
		n++;
	}

	x = c->nx;
	y = c->ny;
	w = c->width;
	h = c->height;
	if (w1 || h1) {
		x = x1;
		y = y1;
		w = w1;
		h = h1;
	}

	/* try to grow upward. locate the lower edge of the nearest fully
	 * visible window
	 */
	for (n = c->phy->yoff, i = 0; i < relevant; i++)
		if (regions[i].y + regions[i].h <= y &&
			OVERLAP(x, w, regions[i].x, regions[i].w))
			n = MAX(n, regions[i].y + regions[i].h);

	h += (y - n) - c->border;
	y = n + c->border;

	/* try to grow downward. locate the upper edge of the nearest
	 * fully visible window
	 */
	for (n = c->phy->yoff + c->phy->height, i = 0; i < relevant; i++)
		if (regions[i].y >= y + h &&
		    OVERLAP(x, w, regions[i].x, regions[i].w))
			n = MIN(n, regions[i].y);

	h = (n - y) - c->border * 2;

	/* try to grow left. locate the right edge of the nearest fully visible
	 * window.
	 */
	for (n = c->phy->xoff, i = 0; i < relevant; i++)
		if (regions[i].x + regions[i].w <= x &&
		    OVERLAP(y, h, regions[i].y, regions[i].h))
			n = MAX(n, regions[i].x + regions[i].w);

	w += (x - n) - c->border;
	x = n + c->border;

	/* try to grow right. locate the left edge of the nearest fully visible
	 * window
	 */
	for (n = c->phy->xoff + c->phy->width, i = 0; i < relevant; i++)
		if (regions[i].x >= x + w &&
		    OVERLAP(y, h, regions[i].y, regions[i].h))
			n = MIN(n, regions[i].x);

	w = (n - x) - c->border;

	c->nx = x;
	c->ny = y;
	c->height = h;
	c->width = w;

	fprintf(stderr, "w: %d, h: %d, x: %d, y: %d", w, h, x, y);

	moveresize(c);
}

void
drag(struct client *c)
{
	XEvent      ev;
	int         x1, y1;	/* pointer position at start of grab in screen co-ordinates */
	int         old_screen_x = client_to_Xcoord(c, x);
	int         old_screen_y = client_to_Xcoord(c, y);;

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
				int         screen_x =
					old_screen_x + (ev.xmotion.x - x1);
				int         screen_y =
					old_screen_y + (ev.xmotion.y - y1);
				client_update_screenpos(c, screen_x, screen_y);
				client_calc_phy(c);
				if (opt_snap && !(ev.xmotion.state & altmask))
					snap_client(c);

				if (!no_solid_drag) {
					XMoveWindow(dpy, c->parent,
						client_to_Xcoord(c,
							x) - c->border,
						client_to_Xcoord(c,
							y) - c->border);
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
	c->nx = MAX(1 - c->width - c->border, MIN(c->nx, c->phy->width));
	c->ny = MAX(1 - c->height - c->border, MIN(c->ny, c->phy->height));
}

void
moveresize(struct client * c)
{
	position_policy(c);
	XMoveResizeWindow(dpy, c->parent,
		client_to_Xcoord(c, x) - c->border, client_to_Xcoord(c,
			y) - c->border, c->width, c->height);
	XMoveResizeWindow(dpy, c->window, 0, 0, c->width, c->height);
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
	if (hv & MAXIMISE_FULLSCREEN)
		hv |= MAXIMISE_HORZ|MAXIMISE_VERT;

	if (hv & MAXIMISE_HORZ) {
		if (c->oldw) {
			if (action == NET_WM_STATE_REMOVE
				|| action == NET_WM_STATE_TOGGLE) {
				c->nx = c->oldx;
				c->width = c->oldw;
				c->oldw = 0;
				XDeleteProperty(dpy, c->window,
					xa_evilwm_unmaximised_horz);
			}
		} else {
			if (action == NET_WM_STATE_ADD
				|| action == NET_WM_STATE_TOGGLE) {
				unsigned long props[2];

				c->oldx = c->nx;
				c->oldw = c->width;
				c->nx = 0 + c->border;
				c->width = c->phy->width - c->border * 2;
				props[0] = c->oldx;
				props[1] = c->oldw;
				XChangeProperty(dpy, c->window,
					xa_evilwm_unmaximised_horz,
					XA_CARDINAL, 32, PropModeReplace,
					(unsigned char *) &props, 2);
			}
		}
	}
	if (hv & MAXIMISE_VERT) {
		if (c->oldh) {
			if (action == NET_WM_STATE_REMOVE
				|| action == NET_WM_STATE_TOGGLE) {
				c->ny = c->oldy;
				c->height = c->oldh;
				c->oldh = 0;
				XDeleteProperty(dpy, c->window,
					xa_evilwm_unmaximised_vert);
			}
		} else {
			if (action == NET_WM_STATE_ADD
				|| action == NET_WM_STATE_TOGGLE) {
				unsigned long props[2];

				c->oldy = c->ny;
				c->oldh = c->height;
				c->ny = 0 + c->border;
				c->height = c->phy->height - c->border * 2;
				props[0] = c->oldy;
				props[1] = c->oldh;
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
				c->old_border = c->border;
				c->border = 0;
				c->nx = c->ny = 0;
				c->width = c->phy->width;
				c->height = c->phy->height;
				XSetWindowBorderWidth(dpy, c->parent, 0);
			} else {

				XSetWindowBorderWidth(dpy, c->parent,
					c->old_border);
				c->border = c->old_border;
			}
		} else {
			XSetWindowBorderWidth(dpy, c->parent, c->old_border);
			c->border = c->old_border;
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
	setmouse(newc->window, newc->width + newc->border - 1,
		newc->height + newc->border - 1);
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
	int         oldw = old_phy->width;
	int         oldh = old_phy->height;
	int         neww = c->phy->width;
	int         newh = c->phy->height;

	if (c->oldw) {
		/* horiz maximised: update width, update old x pos */
		c->width = neww;
		c->oldx = scale_pos(neww, oldw, c->oldx, c->oldw, c->border);
	} else {
		/* horiz normal: update x pos */
		c->nx = scale_pos(neww, oldw, c->nx, c->width, c->border);
	}

	if (c->oldh) {
		/* vert maximised: update height, update old y pos */
		c->height = newh;
		c->oldy = scale_pos(newh, oldh, c->oldy, c->oldh, c->border);
	} else {
		/* vert normal: update y pos */
		c->ny = scale_pos(newh, oldh, c->ny, c->height, c->border);
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
<<<<<<< HEAD
	struct physical_screen *phy = NULL;
=======
	PhysicalScreen *phy = NULL;
	unsigned int	i = 0;
>>>>>>> f2bafdf... First stab at expand; logic mostly from goomwwm

	/* Find if (screen_x,y) is on any physical screen */
	for (i = 0; i < (unsigned) screen->num_physical; i++) {
		phy = &screen->physical[i];
		if (screen_x >= phy->xoff && screen_x <= phy->xoff + phy->width
			&& screen_y >= phy->yoff
			&& screen_y <= phy->yoff + phy->height)
			return phy;
	}

	/* fall back to finding the closest screen minimum distance between the
	 * physical screen centre to (screen_x,y) */
	int         val = INT_MAX;

<<<<<<< HEAD
	for (unsigned i = 0; i < (unsigned) screen->num_physical; i++) {
		struct physical_screen *p = &screen->physical[i];
=======
	for (i = 0; i < (unsigned) screen->num_physical; i++) {
		PhysicalScreen *p = &screen->physical[i];
>>>>>>> f2bafdf... First stab at expand; logic mostly from goomwwm

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
		KEY_FULLSCREEN, KEY_DOCK_TOGGLE, KEY_EXPAND
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
