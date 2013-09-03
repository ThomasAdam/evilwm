/* evilwm - Minimalist Window Manager for X
 * Copyright (C) 1999-2009 Ciaran Anscomb <evilwm@6809.org.uk>
 * see README for license and other details. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "evilwm.h"

#ifdef PANGO
#include <X11/Xft/Xft.h>
#include <pango/pango.h>
#include <pango/pangoxft.h>
#endif

/*
 * Infobanner window functions
 */

static Window info_window = None;
static void infobanner_create(struct client * c);
static void infobanner_update(struct client * c);
static void infobanner_remove(struct client * c);

#ifdef PANGO
static XftDraw *info_xft_draw = NULL;
static PangoRenderer *info_pr = NULL;
static PangoContext *info_pc = NULL;
static PangoLayout *info_pl = NULL;
#endif

static void
infobanner_create(struct client * c)
{
	struct geometry	 g;

	assert(info_window == None);

	g = c->current;

	info_window = XCreateWindow(dpy, c->screen->root, -4, -4, 2, 2, 0,
		CopyFromParent, InputOutput, CopyFromParent,
		CWSaveUnder | CWBackPixel, &(XSetWindowAttributes) {
		.background_pixel = c->screen->fg.pixel,.save_under =
			True});
#ifdef PANGO
	Visual     *v = DefaultVisual(dpy, 0);
	Colormap    cmap = DefaultColormap(dpy, 0);

	info_xft_draw = XftDrawCreate(dpy, info_window, v, cmap);

	if (!info_pc)
		info_pc = pango_xft_get_context(dpy, 0);

	PangoColor  colour;

	pango_color_parse(&colour, "#255adf");

	info_pr = pango_xft_renderer_new(dpy, 0);
	pango_xft_renderer_set_draw((PangoXftRenderer *) info_pr,
		info_xft_draw);
	pango_xft_renderer_set_default_color((PangoXftRenderer *) info_pr,
		&colour);

	info_pl = pango_layout_new(info_pc);
	PangoFontDescription *pd =
		pango_font_description_from_string("Sans 12");
	pango_layout_set_font_description(info_pl, pd);
	pango_layout_set_width(info_pl, g.w * PANGO_SCALE);
	pango_font_description_free(pd);
#endif

	XMapRaised(dpy, info_window);
	infobanner_update(c);
}

static void
fetch_utf8_name(Display * d, Window w, char **name)
{
#ifndef NOUTF8
	Atom        actual_type;
	int         format;
	unsigned long dummy;
	unsigned long name_len;

	XGetWindowProperty(d, w, xa_net_wm_name, 0, -1, False,
		xa_utf8_string, &actual_type, &format,
		&name_len, &dummy, (unsigned char **) name);
#define _NOUTF8 0
#else
#define _NOUTF8 1
#endif
	if (_NOUTF8 || !*name) {
		XFetchName(dpy, w, name);
	}
}

static void
infobanner_update(struct client * c)
{
	struct geometry	 g;
	char		*name;
	char		 buf[27];
	int		 iwinx, iwiny, iwinw, iwinh;
	int		 width_inc = c->hints.width_inc;
	int		 height_inc = c->hints.height_inc;

	if (!info_window)
		return;

	g = c->current;

	snprintf(buf, sizeof(buf), "%dx%d+%d+%d",
		(g.w - c->hints.base_width) / width_inc,
		(g.h - c->hints.base_height) / height_inc,
		client_to_Xcoord(c,x), client_to_Xcoord(c, y));
	iwinw = XTextWidth(font, buf, strlen(buf)) + 2;
	iwinh = font->max_bounds.ascent + font->max_bounds.descent;

	fetch_utf8_name(dpy, c->window, &name);

	if (name) {
#ifdef PANGO
		int         namew, nameh;

		pango_layout_set_text(info_pl, name, -1);
		pango_layout_get_size(info_pl, &namew, &nameh);
		namew = PANGO_PIXELS_CEIL(namew);
		nameh = PANGO_PIXELS_CEIL(nameh);
#else
		int         namew = XTextWidth(font, name, strlen(name));
		int         nameh = iwinh;
#endif
		if (namew > iwinw)
			iwinw = namew + 2;
		iwinh += nameh;
	}

	iwinx = g.x + g.border_width + g.w - iwinw;
	iwiny = g.y - g.border_width;
	if (iwinx + iwinw > c->phy->width)
		iwinx = c->phy->width - iwinw;
	if (iwinx < 0)
		iwinx = 0;
	if (iwiny + iwinh > c->phy->height)
		iwiny = c->phy->height - iwinh;
	if (iwiny < 0)
		iwiny = 0;
	/* convert to X11 logical screen co-ordinates */
	iwinx += c->phy->xoff;
	iwiny += c->phy->yoff;
	XMoveResizeWindow(dpy, info_window, iwinx, iwiny, iwinw, iwinh);
	XClearWindow(dpy, info_window);
	if (name) {
#ifdef PANGO
		pango_renderer_draw_layout(info_pr, info_pl, 0, 0);
#else
		XDrawString(dpy, info_window, c->screen->invert_gc,
			1, iwinh / 2 - 1, name, strlen(name));
#endif
		XFree(name);
	}
	XDrawString(dpy, info_window, c->screen->invert_gc, 1, iwinh - 1,
		buf, strlen(buf));
}

static void
infobanner_remove(struct client * c)
{
	(void) c;
	if (info_window) {
#ifdef PANGO
		XftDrawDestroy(info_xft_draw);
#endif
		XDestroyWindow(dpy, info_window);
	}
	info_window = None;
}

/*
 * XOR decoration functions
 */

static void
xor_draw_outline(struct client * c)
{
	struct geometry	 g;
	int		 screen_x = client_to_Xcoord(c, x);
	int		 screen_y = client_to_Xcoord(c, y);

	g = c->current;

	XDrawRectangle(dpy, c->screen->root, c->screen->invert_gc,
		screen_x - g.border_width, screen_y - g.border_width,
		g.w + 2 * g.border_width - 1, g.h + 2 * g.border_width - 1);
}

static void
xor_draw_info(struct client * c)
{
	struct geometry	 g;
	int		 screen_x = client_to_Xcoord(c, x);
	int		 screen_y = client_to_Xcoord(c, y);
	char		 buf[27];

	g = c->current;

	snprintf(buf, sizeof(buf), "%dx%d+%d+%d",
		(g.w - c->hints.base_width) / c->hints.width_inc,
		(g.h - c->hints.base_height) / c->hints.height_inc,
		screen_x, screen_y);
	XDrawString(dpy, c->screen->root, c->screen->invert_gc,
		screen_x + g.w - XTextWidth(font, buf, strlen(buf)) -
		SPACE, screen_y + g.h - SPACE, buf, strlen(buf));
}

static void
xor_draw_cog(struct client * c)
{
	int         screen_x = client_to_Xcoord(c, x);
	int         screen_y = client_to_Xcoord(c, y);

	/* draw a cross-hair representing the client's centre of gravity */
	int         cog_screen_x = screen_x + c->cog.x;
	int         cog_screen_y = screen_y + c->cog.y;

	XDrawLine(dpy, c->screen->root, c->screen->invert_gc,
		cog_screen_x - 4, cog_screen_y,
		cog_screen_x + 5, cog_screen_y);
	XDrawLine(dpy, c->screen->root, c->screen->invert_gc,
		cog_screen_x, cog_screen_y - 4,
		cog_screen_x, cog_screen_y + 5);
}

static unsigned grabbed = 0;
static void
xor_init(void)
{
	if (!grabbed++) {
		XGrabServer(dpy);
	}
}

static void
xor_fini(void)
{
	if (!--grabbed) {
		XUngrabServer(dpy);
		XSync(dpy, False);
	}
}

#define xor_template(name) \
	static void xor_ ## name ## _create(struct client *c) { \
		xor_init(); \
		xor_draw_ ## name (c); \
	} \
	static void xor_ ## name ## _remove(struct client *c) { \
		xor_draw_ ## name (c); \
		xor_fini(); \
	}

xor_template(outline);
xor_template(info);
xor_template(cog);

/*
 * XShape decoration functions
 */
#ifdef SHAPE
static Window shape_outline_window = None;
static unsigned shape_outline_serial = 0;
static unsigned shape_outline_width;
static unsigned shape_outline_height;

static void
shape_outline_shape(struct client * c)
{
	(void) c;
	unsigned    width = shape_outline_width;
	unsigned    height = shape_outline_height;

	Region      r = XCreateRegion();
	Region      r_in = XCreateRegion();
	XRectangle  rect = {.x = 0,.y = 0,.width = width,.height = height };
	XUnionRectWithRegion(&rect, r, r);
	rect.x = rect.y = 1 /* or could be: * c->border */ ;
	rect.width -= 2 /* or could be: * c->border */ ;
	rect.height -= 2 /* or could be: * c->border */ ;
	XUnionRectWithRegion(&rect, r_in, r_in);
	XSubtractRegion(r, r_in, r);
	XShapeCombineRegion(dpy, shape_outline_window, ShapeBounding, 0, 0, r,
		ShapeSet);

	shape_outline_serial++;
}

static void
shape_outline_create(struct client * c)
{
	struct geometry	 g;
	int		 screen_x;
	int		 screen_y;
	unsigned	 width;
	unsigned	 height;

	if (shape_outline_window != None)
		return;

	g = c->current;
	width = g.w + 2 * g.border_width;
	height = g.h + 2 * g.border_width;
	screen_x = client_to_Xcoord(c, x) - g.border_width;
	screen_y = client_to_Xcoord(c, y) - g.border_width;

	/* cache width & height */
	shape_outline_width = width;
	shape_outline_height = height;

	shape_outline_window =
		XCreateWindow(dpy, c->screen->root, screen_x, screen_y, width,
		height, 0, CopyFromParent, InputOutput, CopyFromParent,
		CWSaveUnder | CWBackPixel,
		&(XSetWindowAttributes) {
			.background_pixel = c->screen->fg.pixel,
			.save_under =True
		});

	shape_outline_shape(c);
	XMapRaised(dpy, shape_outline_window);
}

static void
shape_outline_remove(struct client * c)
{
	(void) c;
	if (shape_outline_window != None)
		XDestroyWindow(dpy, shape_outline_window);
	shape_outline_window = None;
}

static void
shape_outline_update(struct client * c)
{
	struct geometry	 g;
	int		 screen_x;
	int		 screen_y;
	unsigned	 width;
	unsigned	 height;

	g = c->current;

	width = g.w + 2 * g.border_width;
	height = g.h + 2 * g.border_width;
	screen_x = client_to_Xcoord(c, x) - g.border_width;
	screen_y = client_to_Xcoord(c, y) - g.border_width;

	XMoveResizeWindow(dpy, shape_outline_window, screen_x, screen_y, width,
		height);
	if (width == shape_outline_width && height == shape_outline_height)
		return;

	shape_outline_width = width;
	shape_outline_height = height;
	shape_outline_shape(c);
}

static void
shape_outline_cog_shape(struct client * c)
{
	Region      r = XCreateRegion();

	XUnionRectWithRegion(&(XRectangle) {
		.x = 0,.y = 4,.width = 9,.height = 1}, r, r);
	XUnionRectWithRegion(&(XRectangle) {
		.x = 4,.y = 0,.width = 1,.height = 9}, r, r);
	XShapeCombineRegion(dpy, shape_outline_window, ShapeBounding,
		c->cog.x - 4, c->cog.y - 4, r, ShapeUnion);
}

static unsigned shape_cog_serial = 0;
static void
shape_cog_create(struct client * c)
{
	shape_outline_create(c);
	shape_outline_cog_shape(c);
	shape_cog_serial = shape_outline_serial;
}

static void
shape_cog_update(struct client * c)
{
	shape_outline_update(c);
	if (shape_cog_serial != shape_outline_serial) {
		shape_outline_cog_shape(c);
		shape_cog_serial = shape_outline_serial;
	}
}
#endif

/*
 * Annotation method tables
 */
typedef struct
{
	void        (*create) (struct client * c);
	void        (*preupdate) (struct client * c);
	void        (*update) (struct client * c);
	void        (*remove) (struct client * c);
} annotate_funcs;

const annotate_funcs x11_infobanner = {
	.create = infobanner_create,
	.preupdate = NULL,
	.update = infobanner_update,
	.remove = infobanner_remove,
};

const annotate_funcs xor_info = {
	.create = xor_info_create,
	.preupdate = xor_info_remove,
	.update = xor_info_create,
	.remove = xor_info_remove,
};

const annotate_funcs xor_outline = {
	.create = xor_outline_create,
	.preupdate = xor_outline_remove,
	.update = xor_outline_create,
	.remove = xor_outline_remove,
};

const annotate_funcs xor_cog = {
	.create = xor_cog_create,
	.preupdate = xor_cog_remove,
	.update = xor_cog_create,
	.remove = xor_cog_remove,
};

#ifdef SHAPE
const annotate_funcs shape_outline = {
	.create = shape_outline_create,
	.preupdate = NULL,
	.update = shape_outline_update,
	.remove = shape_outline_remove,
};

const annotate_funcs shape_cog = {
	.create = shape_cog_create,
	.preupdate = NULL,
	.update = shape_cog_update,
	.remove = shape_outline_remove,
};
#else
#define shape_outline xor_outline
#define shape_cog xor_cog
#endif

/* compile time defaults */
#ifdef INFOBANNER
#define ANNOTATE_INFOBANNER x11_infobanner
#else
#define ANNOTATE_INFOBANNER xor_info
#endif

#ifdef INFOBANNER_MOVERESIZE
#define ANNOTATE_MOVERESIZE x11_infobanner
#else
#define ANNOTATE_MOVERESIZE xor_info
#endif

struct annotate_ctx
{
	const annotate_funcs *outline;
	const annotate_funcs *info;
	const annotate_funcs *cog;
};
typedef struct annotate_ctx annotate_ctx_t;

annotate_ctx_t annotate_info_ctx = { NULL, &ANNOTATE_INFOBANNER, NULL };
annotate_ctx_t annotate_drag_ctx =
	{ &shape_outline, &ANNOTATE_MOVERESIZE, &shape_cog };
annotate_ctx_t annotate_sweep_ctx =
	{ &shape_outline, &ANNOTATE_MOVERESIZE, &shape_cog };

/*
 * Annotation functions
 */
#define annotate_template(name) \
	void annotate_ ## name (struct client *c, annotate_ctx_t *a) { \
		if (!a) return; \
		if (a->outline && a->outline->name) a->outline->name(c); \
		if (a->info && a->info->name) a->info->name(c); \
		if (a->cog && a->cog->name) a->cog->name(c); \
	}
annotate_template(create);
annotate_template(preupdate);
annotate_template(update);
annotate_template(remove);

/* setter functions for config */

static const annotate_funcs *
name_to_annotateobj(const char *name)
{
	if (!strcmp(name, "x11_infobanner"))
		return &x11_infobanner;
	if (!strcmp(name, "xor_info"))
		return &xor_info;
	if (!strcmp(name, "xor_outline"))
		return &xor_outline;
	if (!strcmp(name, "xor_cog"))
		return &xor_cog;
	if (!strcmp(name, "shape_outline"))
		return &shape_outline;
	if (!strcmp(name, "shape_cog"))
		return &shape_cog;
	return NULL;
}

#define set_annotate_template(name, target) \
void set_annotate_ ## name ## _ ## target (const char* arg) { \
	annotate_ ## name ## _ctx.target = name_to_annotateobj(arg); \
}

set_annotate_template(info, outline)
	set_annotate_template(info, info)
	set_annotate_template(info, cog)
	set_annotate_template(drag, outline)
	set_annotate_template(drag, info)
	set_annotate_template(drag, cog)
	set_annotate_template(sweep, outline)
	set_annotate_template(sweep, info)
	set_annotate_template(sweep, cog)
