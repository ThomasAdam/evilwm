/* evilwm - Minimalist Window Manager for X
 * Copyright (C) 1999-2011 Ciaran Anscomb <evilwm@6809.org.uk>
 * see README for license and other details. */

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xmd.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#ifdef SHAPE
#include <X11/extensions/shape.h>
#endif
#ifdef RANDR
#include <X11/extensions/Xrandr.h>
#endif
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif

#include <stdbool.h>

#ifndef __GNUC__
#define  __attribute__(x)
#endif

#include "keymap.h"
#include "list.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define NEAR(a,o,b) ((b) > (a)-(o) && (b) < (a)+(o))
#define SNAPTO(a,o,b,j) (NEAR((a),(o),(b)) ? (a): (b)+(j))
#define OVERLAP(a,b,c,d) (((a)==(c) && (b)==(d)) || MIN((a)+(b), (c)+(d)) - MAX((a), (c)) > 0)
#define INTERSECT(x,y,w,h,x1,y1,w1,h1) (OVERLAP((x),(w),(x1),(w1)) && OVERLAP((y),(h),(y1),(h1)))


/* Required for interpreting MWM hints: */
#define _XA_MWM_HINTS           "_MOTIF_WM_HINTS"
#define PROP_MWM_HINTS_ELEMENTS 3
#define MWM_HINTS_DECORATIONS   (1L << 1)
#define MWM_DECOR_ALL           (1L << 0)
#define MWM_DECOR_BORDER        (1L << 1)
typedef struct
{
	unsigned long flags;
	unsigned long functions;
	unsigned long decorations;
} PropMwmHints;

typedef struct {
	short x, y, w, h, l, r, t, b;
} workarea;


/* sanity on options */
#if defined(INFOBANNER_MOVERESIZE) && !defined(INFOBANNER)
#define INFOBANNER
#endif

/* default settings */

#define DEF_FONT        "variable"
#define DEF_FG          "goldenrod"
#define DEF_BG          "grey50"
#define DEF_BW          1
#define DEF_FC          "blue"
#define SPACE           3
#ifdef DEBIAN
#define DEF_TERM        "x-terminal-emulator"
#else
#define DEF_TERM        "xterm"
#endif

/* readability stuff */

#define VDESK_INVALID (0xfffffffd)
#define VDESK_NONE  (0xfffffffe)
#define VDESK_FIXED (0xffffffff)
#define VDESK_MAX   (opt_vdesks - 1)
#define KEY_TO_VDESK(key) (((key) - XK_1 + 10) % 10)
#define valid_vdesk(v) ((v) == VDESK_FIXED || (v) < opt_vdesks)

#define RAISE           1
#define NO_RAISE        0	/* for unhide() */

/* EWMH hints use these definitions, so for simplicity my functions
 * will too: */
#define NET_WM_STATE_REMOVE     0	/* remove/unset property */
#define NET_WM_STATE_ADD        1	/* add/set property */
#define NET_WM_STATE_TOGGLE     2	/* toggle property  */

/* EWMH window type bits */
#define EWMH_WINDOW_TYPE_DESKTOP (1<<0)
#define EWMH_WINDOW_TYPE_DOCK    (1<<1)

#define MAXIMISE_HORZ		(1<<0)
#define MAXIMISE_VERT		(1<<1)
#define MAXIMISE_FULLSCREEN	(1<<2)

/* some coding shorthand */

#define ChildMask       (SubstructureRedirectMask|SubstructureNotifyMask)
#define ButtonMask      (ButtonPressMask|ButtonReleaseMask)
#define MouseMask       (ButtonMask|PointerMotionMask)

#define grab_pointer(w, mask, curs) \
	(XGrabPointer(dpy, w, False, mask, GrabModeAsync, GrabModeAsync, \
	None, curs, CurrentTime) == GrabSuccess)
#define grab_button(w, mask, button) do { \
		XGrabButton(dpy, button, (mask), w, False, ButtonMask, \
		            GrabModeAsync, GrabModeSync, None, None); \
		XGrabButton(dpy, button, LockMask|(mask), w, False, ButtonMask,\
		            GrabModeAsync, GrabModeSync, None, None); \
		XGrabButton(dpy, button, numlockmask|(mask), w, False, \
		            ButtonMask, GrabModeAsync, GrabModeSync, \
		            None, None); \
		XGrabButton(dpy, button, numlockmask|LockMask|(mask), w, False,\
		            ButtonMask, GrabModeAsync, GrabModeSync, \
		            None, None); \
	} while (0)
#define setmouse(w, x, y) XWarpPointer(dpy, None, w, 0, 0, 0, 0, x, y)
#define get_mouse_position(xp,yp,root) do { \
		Window dw; \
		int di; \
		unsigned int dui; \
		XQueryPointer(dpy, root, &dw, &dw, xp, yp, &di, &di, &dui); \
	} while (0)

#define is_fixed(c) (c->vdesk == VDESK_FIXED)
#define add_fixed(c) c->vdesk = VDESK_FIXED
#define remove_fixed(c) c->vdesk = c->screen->vdesk

/* screen structure */

/* The Xinerama extension informs us of the Physical Screens that
 * make up a Logical Screen (thing with a root window) */
struct physical_screen
{
	int         xoff;	/* x pos of the physical screen in logical screen coordinates */
	int         yoff;	/* y pos of the physical screen in logical screen coordinates */
	int         width;	/* width of the screen */
	int         height;	/* height of the screen */
	unsigned int vdesk;	/* virtual desktop displayed on this physical screen */
};

struct screen_info
{
	int         screen;
	Window      root;
	Window      supporting;	/* Dummy window for EWMH */
	GC          invert_gc;
	XColor      fg, bg;
	XColor      fc;
	char       *display;
	int         docks_visible;
	unsigned int old_vdesk;	/* most recently unmapped vdesk, so user may toggle back to it */

	int         num_physical;	/* Number of entries in @physical@ */
	struct physical_screen *physical;	/* Physical screens that make up this screen */
};

/* client structure */
struct client
{
	Window      window;
	Window      parent;
	struct screen_info *screen;
	struct physical_screen *phy;	/* the physical screen the client is on. */
	Colormap    cmap;
	int         ignore_unmap;

	int         nx, ny, width, height;
	int         border;
	int         oldx, oldy, oldw, oldh;	/* used when maximising */

	XPoint      cog;	/* client's centre of gravity */

	int         min_width, min_height;
	int         max_width, max_height;
	int         width_inc, height_inc;
	int         base_width, base_height;
	int         win_gravity_hint;
	int         win_gravity;
	int         old_border;
	unsigned int vdesk;
	int         is_dock;
	int         remove;	/* set when client needs to be removed */
};

struct application
{
	char       *res_name;
	char       *res_class;
	int         geometry_mask;
	int         x, y;
	unsigned int width, height;
	int         is_dock;
	unsigned int vdesk;
};

/* Declarations for global variables in main.c */

/* Commonly used X information */
extern Display *dpy;
extern XFontStruct *font;
extern Cursor move_curs;
extern Cursor resize_curs;
extern int  num_screens;
extern struct screen_info *screens;

#ifdef SHAPE
extern int  have_shape, shape_event;
#endif
#ifdef RANDR
extern int  have_randr, randr_event_base;
#endif
#ifdef XINERAMA
extern int  have_xinerama;
#endif

/* Standard X protocol atoms */
extern Atom xa_wm_state;
extern Atom xa_wm_protos;
extern Atom xa_wm_delete;
extern Atom xa_wm_cmapwins;

extern Atom xa_utf8_string;

/* Motif atoms */
extern Atom mwm_hints;

/* evilwm atoms */
extern Atom xa_evilwm_unmaximised_horz;
extern Atom xa_evilwm_unmaximised_vert;
extern Atom xa_evilwm_current_desktops;

/* EWMH: Root Window Properties (and Related Messages) */
extern Atom xa_net_current_desktop;
extern Atom xa_net_active_window;

/* EWMH: Other Root Window Messages */
extern Atom xa_net_close_window;
extern Atom xa_net_moveresize_window;
extern Atom xa_net_restack_window;
extern Atom xa_net_request_frame_extents;

/* EWMH: Application Window Properties */
extern Atom xa_net_wm_name;
extern Atom xa_net_wm_desktop;
extern Atom xa_net_wm_window_type;
extern Atom xa_net_wm_window_type_dock;
extern Atom xa_net_wm_state;
extern Atom xa_net_wm_state_maximized_vert;
extern Atom xa_net_wm_state_maximized_horz;
extern Atom xa_net_wm_state_fullscreen;
extern Atom xa_net_frame_extents;

/* Things that affect user interaction */
extern unsigned int numlockmask;
extern unsigned int grabmask1;
extern unsigned int grabmask2;
extern unsigned int altmask;
extern KeySym opt_key_kill;
extern char **opt_term;
extern int  opt_bw;
extern int  opt_snap;

#ifdef SOLIDDRAG
extern int  no_solid_drag;
#else
#define no_solid_drag (1)
#endif
extern struct list *applications;
extern unsigned int opt_vdesks;	/* number of virtual desktops to use */

/* struct client tracking information */
extern struct list *clients_tab_order;
extern struct list *clients_mapping_order;
extern struct list *clients_stacking_order;
extern struct client *current;
extern volatile Window initialising;

/* Event loop will run until this flag is set */
extern int  wm_exit;

/* client.c */
#define client_to_Xcoord(c,T) (c->phy-> T ## off + c-> n ## T)
#define client_from_Xcoord(c,T,value) do { c-> n ## T = value - c->phy-> T ## off; } while (0)
struct client     *find_client(Window w);
void        client_hide(struct client * c);
void        client_show(struct client * c);
void        client_raise(struct client * c);
void        client_lower(struct client * c);
void        client_update_screenpos(struct client * c, int screen_x, int screen_y);
void        gravitate_border(struct client * c, int bw);
void        select_client(struct client * c);
void        client_to_vdesk(struct client * c, unsigned int vdesk);
void        remove_client(struct client * c);
void        send_config(struct client * c);
void        send_wm_delete(struct client * c, int kill_client);
void        set_wm_state(struct client * c, int state);
void        set_shape(struct client * c);
void       *get_property(Window w, Atom property, Atom req_type,
	unsigned long *nitems_return);
void        client_calc_cog(struct client *);
void        client_calc_phy(struct client *);
void	    client_expand(struct client *);

/* events.c */

void        event_main_loop(void);

/* misc.c */

extern int  need_client_tidy;
extern int  ignore_xerror;
int         handle_xerror(Display * dsply, XErrorEvent * e);
void        spawn(const char *const cmd[]);
void        handle_signal(int signo);
void        discard_enter_events(struct client * except);

/* new.c */

void        make_new_client(Window w, struct screen_info * s);
long        get_wm_normal_hints(struct client * c);
void        get_window_type(struct client * c);

/* screen.c */

void        drag(struct client * c);
void        position_policy(struct client * c);
void        moveresizeraise(struct client * c);
void        moveresize(struct client * c);
void        maximise_client(struct client * c, int action, int hv);
void        show_info(struct client * c, unsigned int keycode);
void        sweep(struct client * c);
void        next(void);
bool        switch_vdesk(struct screen_info * s, struct physical_screen * p, unsigned int v);
void        exchange_phy(struct screen_info * s);
void        set_docks_visible(struct screen_info * s, int is_visible);
struct screen_info *find_screen(Window root);
struct screen_info *find_current_screen(void);
void        find_current_screen_and_phy(struct screen_info ** current_screen,
	struct physical_screen ** current_phy);
struct physical_screen *find_physical_screen(struct screen_info * screen, int x, int y);
void        grab_keys_for_screen(struct screen_info * s);
void        probe_screen(struct screen_info * s);

/* ewmh.c */

void        ewmh_init(void);
void        ewmh_init_screen(struct screen_info * s);
void        ewmh_deinit_screen(struct screen_info * s);
void        ewmh_set_screen_workarea(struct screen_info * s);
void        ewmh_init_client(struct client * c);
void        ewmh_deinit_client(struct client * c);
void        ewmh_withdraw_client(struct client * c);
void        ewmh_select_client(struct client * c);
void        ewmh_set_net_client_list(struct screen_info * s);
void        ewmh_set_net_client_list_stacking(struct screen_info * s);
void        ewmh_set_net_current_desktop(struct screen_info * s);
void        ewmh_set_net_active_window(struct client * c);
void        ewmh_set_net_wm_desktop(struct client * c);
unsigned int ewmh_get_net_wm_window_type(Window w);
void        ewmh_set_net_wm_state(struct client * c);
void        ewmh_set_net_frame_extents(Window w);

/* annotations.c */

struct annotate_ctx;
extern struct annotate_ctx annotate_info_ctx;
extern struct annotate_ctx annotate_drag_ctx;
extern struct annotate_ctx annotate_sweep_ctx;
void        annotate_create(struct client * c, struct annotate_ctx *a);
void        annotate_preupdate(struct client * c, struct annotate_ctx *a);
void        annotate_update(struct client * c, struct annotate_ctx *a);
void        annotate_remove(struct client * c, struct annotate_ctx *a);
void        set_annotate_info_outline(const char *arg);
void        set_annotate_info_info(const char *arg);
void        set_annotate_info_cog(const char *arg);
void        set_annotate_drag_outline(const char *arg);
void        set_annotate_drag_info(const char *arg);
void        set_annotate_drag_cog(const char *arg);
void        set_annotate_sweep_outline(const char *arg);
void        set_annotate_sweep_info(const char *arg);
void        set_annotate_sweep_cog(const char *arg);

/* defines */
static inline int
should_be_mapped(struct client * c)
{
	if (is_fixed(c))
		return 1;
	/* xxx, dock */
	for (unsigned i = 0; i < (unsigned) c->screen->num_physical; i++) {
		if (c->vdesk == c->screen->physical[i].vdesk)
			return 1;
	}
	return 0;
}
