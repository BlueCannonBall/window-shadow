/*
 * window-shadow: X11 shadow daemon for CSD windows
 *
 * Renders drop shadows behind Electron and other client-side-decorated
 * windows that don't provide their own shadows.
 *
 * Requires a running compositor for ARGB transparency.
 *
 * Dependencies: libX11, libXext, libXfixes, libXrender, cairo
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Motif WM hints ──────────────────────────────────────────────── */

#define MWM_HINTS_DECORATIONS (1L << 1)

typedef struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
} MotifWmHints;

/* ── Shadow entry (linked list) ──────────────────────────────────── */

typedef struct ShadowEntry {
    Window toplevel; /* direct child of root (frame or client) */
    Window client;   /* actual client window */
    Window shadow;   /* our shadow window */
    Pixmap pixmap;   /* stored shadow pixmap for Expose redraws */
    int x, y, w, h;  /* toplevel geometry */
    int sw, sh;      /* shadow window size */
    int is_active;   /* focus state */
    
    /* Cache for full, un-clamped shadow surface */
    cairo_surface_t* full_shadow;
    uint32_t* full_shadow_data;
    int last_w, last_h, last_active;
    
    struct ShadowEntry* next;
} ShadowEntry;

/* ── Globals ─────────────────────────────────────────────────────── */

static Display* dpy;
static int scr;
static int screen_w, screen_h;
static Window root;
static Visual* argb_visual;
static Colormap argb_cmap;
static ShadowEntry* shadow_list;
static volatile int running = 1;

static int cached_wx = 0, cached_wy = 0, cached_ww = 0, cached_wh = 0;

static void update_workarea_cache(void) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char* data = NULL;

    if (XGetWindowProperty(dpy, root, XInternAtom(dpy, "_NET_WORKAREA", False), 0, 4, False, XA_CARDINAL, &actual_type, &actual_format, &nitems, &bytes_after, &data) == Success && data) {
        long* wa = (long*) data;
        cached_wx = wa[0];
        cached_wy = wa[1];
        cached_ww = wa[2];
        cached_wh = wa[3];
        XFree(data);
    } else {
        cached_wx = 0;
        cached_wy = 0;
        cached_ww = screen_w;
        cached_wh = screen_h;
    }
}

static void get_workarea(int* wx, int* wy, int* ww, int* wh) {
    *wx = cached_wx;
    *wy = cached_wy;
    *ww = cached_ww;
    *wh = cached_wh;
}

/* Atoms */
static Atom A_MOTIF_WM_HINTS;
static Atom A_GTK_FRAME_EXTENTS;
static Atom A_NET_FRAME_EXTENTS;
static Atom A_NET_WM_WINDOW_TYPE;
static Atom A_NET_WM_WINDOW_TYPE_NORMAL;
static Atom A_NET_WM_WINDOW_TYPE_DIALOG;
static Atom A_WM_STATE;
static Atom A_NET_WM_STATE;
static Atom A_NET_WM_STATE_MAXIMIZED_VERT;
static Atom A_NET_WM_STATE_MAXIMIZED_HORZ;
static Atom A_NET_WM_STATE_FULLSCREEN;
static Atom A_NET_WM_STATE_HIDDEN;
static Atom A_NET_WM_STATE_SKIP_TASKBAR;
static Atom A_NET_WM_STATE_SKIP_PAGER;
static Atom A_NET_WM_WINDOW_TYPE_UTILITY;
static Atom A_SHADOW_WINDOW;
static Atom A_NET_RESTACK_WINDOW;
static Atom A_NET_ACTIVE_WINDOW;
static Atom A_NET_WORKAREA;

static int cfg_debug = 0;

static int cfg_radius = 60;
static float cfg_opacity = 0.8f;
static float cfg_inactive_opacity = 0.4f;
static int cfg_offset_x = 0;
static int cfg_offset_y = 8;
static float cfg_color_r = 0.0f;
static float cfg_color_g = 0.0f;
static float cfg_color_b = 0.0f;

/* ── Helpers ─────────────────────────────────────────────────────── */

static int x_error_handler(Display* d, XErrorEvent* ev) {
    (void) d;
    (void) ev;
    return 0; /* swallow all errors (windows can vanish mid-operation) */
}

static unsigned char* get_prop(Window w, Atom prop, Atom type, int* fmt, unsigned long* nitems) {
    Atom actual;
    int af;
    unsigned long n, after;
    unsigned char* data = NULL;
    if (XGetWindowProperty(dpy, w, prop, 0, 1024, False, type, &actual, &af, &n, &after, &data) != Success)
        return NULL;
    if (actual == None) {
        if (data) XFree(data);
        return NULL;
    }
    if (fmt) *fmt = af;
    if (nitems) *nitems = n;
    return data;
}

/* ── ARGB visual ─────────────────────────────────────────────────── */

static Visual* find_argb_visual(void) {
    XVisualInfo tpl = {.screen = scr, .depth = 32, .class = TrueColor};
    int n;
    XVisualInfo* vis = XGetVisualInfo(dpy,
        VisualScreenMask | VisualDepthMask | VisualClassMask,
        &tpl,
        &n);
    if (!vis) return NULL;

    Visual* result = NULL;
    for (int i = 0; i < n; i++) {
        XRenderPictFormat* pf = XRenderFindVisualFormat(dpy, vis[i].visual);
        if (pf && pf->type == PictTypeDirect && pf->direct.alphaMask) {
            result = vis[i].visual;
            break;
        }
    }
    XFree(vis);
    return result;
}

/* ── Box blur (3-pass gaussian approximation) ────────────────────── */

static inline unsigned char sample_clamp(const unsigned char* buf,
    int w,
    int h,
    int x,
    int y) {
    if (x < 0)
        x = 0;
    else if (x >= w)
        x = w - 1;
    if (y < 0)
        y = 0;
    else if (y >= h)
        y = h - 1;
    return buf[y * w + x];
}

static void box_blur_h(const unsigned char* src, unsigned char* dst, int w, int h, int r) {
    float inv = 1.0f / (2 * r + 1);
    for (int y = 0; y < h; y++) {
        int sum = 0;
        for (int k = -r; k <= r; k++)
            sum += sample_clamp(src, w, h, k, y);
        dst[y * w] = (unsigned char) (sum * inv);
        for (int x = 1; x < w; x++) {
            sum += sample_clamp(src, w, h, x + r, y) - sample_clamp(src, w, h, x - r - 1, y);
            dst[y * w + x] = (unsigned char) (sum * inv);
        }
    }
}

static void box_blur_v(const unsigned char* src, unsigned char* dst, int w, int h, int r) {
    float inv = 1.0f / (2 * r + 1);
    for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int k = -r; k <= r; k++)
            sum += sample_clamp(src, w, h, x, k);
        dst[x] = (unsigned char) (sum * inv);
        for (int y = 1; y < h; y++) {
            sum += sample_clamp(src, w, h, x, y + r) - sample_clamp(src, w, h, x, y - r - 1);
            dst[y * w + x] = (unsigned char) (sum * inv);
        }
    }
}

static void gaussian_blur(unsigned char* buf, int w, int h, int radius) {
    unsigned char* tmp = malloc(w * h);
    if (!tmp) return;
    int r = radius / 3;
    if (r < 1) r = 1;
    for (int pass = 0; pass < 3; pass++) {
        box_blur_h(buf, tmp, w, h, r);
        box_blur_v(tmp, buf, w, h, r);
    }
    free(tmp);
}

/* ── Render the shadow into a Pixmap and store it in the entry.
 * Also sets the window background and applies XShape bounding.
 */
static void render_shadow(ShadowEntry* e, int tw, int th) {
    /* Calculate clamped geometry */
    int sx = e->x - cfg_radius + cfg_offset_x;
    int sy = e->y - cfg_radius + cfg_offset_y;
    int sw = tw + 2 * cfg_radius;
    int sh = th + 2 * cfg_radius;

    int actual_x = sx;
    int actual_y = sy;
    int actual_w = sw;
    int actual_h = sh;

    int render_offset_x = 0;
    int render_offset_y = 0;

    int wx, wy, ww, wh;
    get_workarea(&wx, &wy, &ww, &wh);

    if (actual_x < wx) {
        actual_w -= (wx - actual_x);
        render_offset_x = (wx - actual_x); /* negative offset */
        actual_x = wx;
    }
    if (actual_y < wy) {
        actual_h -= (wy - actual_y);
        render_offset_y = (wy - actual_y);
        actual_y = wy;
    }
    if (actual_x + actual_w > wx + ww) {
        actual_w = (wx + ww) - actual_x;
    }
    if (actual_y + actual_h > wy + wh) {
        actual_h = (wy + wh) - actual_y;
    }
    if (actual_w <= 0 || actual_h <= 0) {
        actual_w = 1;
        actual_h = 1;
        actual_x = -100;
        actual_y = -100;
    }

    e->sw = actual_w;
    e->sh = actual_h;

    int npx = sw * sh;

    if (e->full_shadow && (e->last_w != tw || e->last_h != th || e->last_active != e->is_active)) {
        cairo_surface_destroy(e->full_shadow);
        free(e->full_shadow_data);
        e->full_shadow = NULL;
        e->full_shadow_data = NULL;
    }

    if (!e->full_shadow) {
        /* Build alpha mask: solid rectangle where the window is */
        unsigned char* alpha = calloc(npx, 1);
        if (!alpha) return;

        for (int y = cfg_radius; y < cfg_radius + th; y++)
            for (int x = cfg_radius; x < cfg_radius + tw; x++)
                alpha[y * sw + x] = 255;

        gaussian_blur(alpha, sw, sh, cfg_radius);

        int cx0 = cfg_radius - cfg_offset_x;
        int cy0 = cfg_radius - cfg_offset_y;

        /* Clear the center (alpha=0) where the target window sits */
        for (int y = cy0; y < cy0 + th; y++)
            for (int x = cx0; x < cx0 + tw; x++)
                if (x >= 0 && x < sw && y >= 0 && y < sh)
                    alpha[y * sw + x] = 0;

        /* Convert to premultiplied ARGB */
        uint32_t* argb = calloc(npx, sizeof(uint32_t));
        if (!argb) {
            free(alpha);
            return;
        }

        uint8_t cr = (uint8_t) (cfg_color_r * 255);
        uint8_t cg = (uint8_t) (cfg_color_g * 255);
        uint8_t cb = (uint8_t) (cfg_color_b * 255);

        float active_opacity = e->is_active ? cfg_opacity : cfg_inactive_opacity;

        for (int i = 0; i < npx; i++) {
            uint8_t a = (uint8_t) (alpha[i] * active_opacity);
            uint8_t r = (uint8_t) (cr * a / 255);
            uint8_t g = (uint8_t) (cg * a / 255);
            uint8_t b = (uint8_t) (cb * a / 255);

            argb[i] = ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
        }

        free(alpha);

        e->full_shadow_data = argb;
        e->full_shadow = cairo_image_surface_create_for_data(
            (unsigned char*) argb, CAIRO_FORMAT_ARGB32, sw, sh, sw * 4);
        
        e->last_w = tw;
        e->last_h = th;
        e->last_active = e->is_active;
    }

    if (e->pixmap) XFreePixmap(dpy, e->pixmap);
    e->pixmap = XCreatePixmap(dpy, e->shadow, actual_w, actual_h, 32);

    cairo_surface_t* xsurf = cairo_xlib_surface_create(
        dpy, e->pixmap, argb_visual, actual_w, actual_h);
    cairo_t* cr2 = cairo_create(xsurf);
    cairo_set_source_surface(cr2, e->full_shadow, -render_offset_x, -render_offset_y);
    cairo_set_operator(cr2, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr2);
    cairo_destroy(cr2);
    cairo_surface_destroy(xsurf);

    XSetWindowBackgroundPixmap(dpy, e->shadow, e->pixmap);
    XClearWindow(dpy, e->shadow);

    int cx0 = cfg_radius - cfg_offset_x;
    int cy0 = cfg_radius - cfg_offset_y;

    /*
     * XShape BOUNDING: remove the center rectangle so that area
     * physically doesn't exist. Other windows show through naturally.
     * We must apply the render_offset_x/y to the XShape rectangles too.
     */
    XRectangle rects[4];
    int mask_cx0 = cx0 - render_offset_x;
    int mask_cy0 = cy0 - render_offset_y;
    int clamp_cx0 = mask_cx0 < 0 ? 0 : mask_cx0;
    int clamp_cy0 = mask_cy0 < 0 ? 0 : mask_cy0;
    int clamp_cx1 = (mask_cx0 + tw > actual_w) ? actual_w : mask_cx0 + tw;
    int clamp_cy1 = (mask_cy0 + th > actual_h) ? actual_h : mask_cy0 + th;

    /* Top strip */
    rects[0].x = 0;
    rects[0].y = 0;
    rects[0].width = actual_w;
    rects[0].height = clamp_cy0;
    /* Bottom strip */
    rects[1].x = 0;
    rects[1].y = clamp_cy1;
    rects[1].width = actual_w;
    rects[1].height = actual_h - clamp_cy1;
    /* Left strip (between top and bottom) */
    rects[2].x = 0;
    rects[2].y = clamp_cy0;
    rects[2].width = clamp_cx0;
    rects[2].height = clamp_cy1 - clamp_cy0;
    /* Right strip (between top and bottom) */
    rects[3].x = clamp_cx1;
    rects[3].y = clamp_cy0;
    rects[3].width = actual_w - clamp_cx1;
    rects[3].height = clamp_cy1 - clamp_cy0;

    XShapeCombineRectangles(dpy, e->shadow, ShapeBounding, 0, 0, rects, 4, ShapeSet, YXBanded);
}

/* ── Shadow window creation / destruction ──────────────────────────────── */

static Window create_shadow_window(int x, int y, int tw, int th) {
    int sw = tw + 2 * cfg_radius;
    int sh = th + 2 * cfg_radius;

    /*
     * Managed window (NOT override-redirect) so it participates in
     * the WM's stacking order. Override-redirect windows always render
     * above managed windows on compositing WMs, which breaks shadows.
     */
    XSetWindowAttributes attrs;
    attrs.override_redirect = False;
    attrs.colormap = argb_cmap;
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.event_mask = ExposureMask;

    Window w = XCreateWindow(dpy, root, x - cfg_radius + cfg_offset_x, y - cfg_radius + cfg_offset_y, sw, sh, 0, 32, InputOutput, argb_visual, CWOverrideRedirect | CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &attrs);

    /* Mark as our shadow window to prevent self-shadowing */
    long marker = 1;
    XChangeProperty(dpy, w, A_SHADOW_WINDOW, XA_CARDINAL, 32, PropModeReplace, (unsigned char*) &marker, 1);

    /* Removed UTILITY type so it stays in NORMAL layer (under taskbars) */

    /* No decorations */
    MotifWmHints mhints = {.flags = MWM_HINTS_DECORATIONS, .decorations = 0};
    XChangeProperty(dpy, w, A_MOTIF_WM_HINTS, A_MOTIF_WM_HINTS, 32, PropModeReplace, (unsigned char*) &mhints, 5);

    /* Skip taskbar and pager */
    Atom states[] = {A_NET_WM_STATE_SKIP_TASKBAR, A_NET_WM_STATE_SKIP_PAGER};
    XChangeProperty(dpy, w, A_NET_WM_STATE, XA_ATOM, 32, PropModeReplace, (unsigned char*) states, 2);

    /* Explicitly tell the WM this window cannot take focus so it skips Alt+Tab */
    XWMHints* wmhints = XAllocWMHints();
    if (wmhints) {
        wmhints->flags = InputHint;
        wmhints->input = False;
        XSetWMHints(dpy, w, wmhints);
        XFree(wmhints);
    }

    /* Click-through: empty input shape */
    XserverRegion rgn = XFixesCreateRegion(dpy, NULL, 0);
    XFixesSetWindowShapeRegion(dpy, w, ShapeInput, 0, 0, rgn);
    XFixesDestroyRegion(dpy, rgn);

    return w;
}

static void stack_shadow_below(ShadowEntry* e) {
    /*
     * Ask the WM to place our shadow below the target via
     * _NET_RESTACK_WINDOW client message + fallback XConfigureWindow.
     */
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.send_event = True;
    ev.xclient.window = e->shadow;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_RESTACK_WINDOW", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1;         /* source: 1 (normal app) */
    ev.xclient.data.l[1] = e->client; /* sibling: use client window, not frame! WMs track clients. */
    ev.xclient.data.l[2] = Below;     /* detail */
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);

    XWindowChanges ch;
    memset(&ch, 0, sizeof(ch));
    ch.sibling = e->toplevel;
    ch.stack_mode = Below;
    XConfigureWindow(dpy, e->shadow, CWSibling | CWStackMode, &ch);
}

/* ── CSD detection ───────────────────────────────────────────────── */

static int is_csd_window(Window w) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return 0;
    if (wa.override_redirect) return 0;

    /* Skip our own shadow windows */
    unsigned long shadow_nitems = 0;
    unsigned char* shadow_data = get_prop(w, A_SHADOW_WINDOW, XA_CARDINAL, NULL, &shadow_nitems);
    if (shadow_data) {
        XFree(shadow_data);
        return 0;
    }

    /* Check window type: only shadow normal / dialog / untyped */
    unsigned long nitems = 0;
    unsigned char* data = get_prop(w, A_NET_WM_WINDOW_TYPE, XA_ATOM, NULL, &nitems);
    if (data && nitems > 0) {
        Atom* types = (Atom*) data;
        int dominated = 0;
        for (unsigned long i = 0; i < nitems; i++) {
            if (types[i] != A_NET_WM_WINDOW_TYPE_NORMAL &&
                types[i] != A_NET_WM_WINDOW_TYPE_DIALOG) {
                dominated = 1;
                break;
            }
        }
        XFree(data);
        if (dominated) return 0;
    } else if (data) {
        XFree(data);
    }

    /* Check _MOTIF_WM_HINTS for decoration == 0 */
    data = get_prop(w, A_MOTIF_WM_HINTS, A_MOTIF_WM_HINTS, NULL, &nitems);
    if (!data || nitems < 3) {
        if (data) XFree(data);
        return 0;
    }

    MotifWmHints* hints = (MotifWmHints*) data;
    int no_decor = (hints->flags & MWM_HINTS_DECORATIONS) && hints->decorations == 0;
    XFree(data);
    if (!no_decor) return 0;

    /* If _GTK_FRAME_EXTENTS are large, the app provides its own shadow */
    data = get_prop(w, A_GTK_FRAME_EXTENTS, XA_CARDINAL, NULL, &nitems);
    if (data && nitems == 4) {
        long* ext = (long*) data;
        if (ext[0] >= 10 || ext[1] >= 10 || ext[2] >= 10 || ext[3] >= 10) {
            if (cfg_debug)
                printf("  skip: _GTK_FRAME_EXTENTS %ld,%ld,%ld,%ld\n",
                    ext[0],
                    ext[1],
                    ext[2],
                    ext[3]);
            XFree(data);
            return 0;
        }
    }
    if (data) XFree(data);

    /* If WM applied _NET_FRAME_EXTENTS with a titlebar, it has SSD */
    data = get_prop(w, A_NET_FRAME_EXTENTS, XA_CARDINAL, NULL, &nitems);
    if (data && nitems == 4) {
        long* ext = (long*) data;
        if (ext[0] > 0 || ext[1] > 0 || ext[2] > 0 || ext[3] > 0) {
            if (cfg_debug)
                printf("  skip: _NET_FRAME_EXTENTS %ld,%ld,%ld,%ld\n",
                    ext[0],
                    ext[1],
                    ext[2],
                    ext[3]);
            XFree(data);
            return 0;
        }
    }
    if (data) XFree(data);

    /* Skip maximized or fullscreen windows — shadows are invisible anyway */
    data = get_prop(w, A_NET_WM_STATE, XA_ATOM, NULL, &nitems);
    if (data && nitems > 0) {
        Atom* states = (Atom*) data;
        int dominated = 0;
        int max_h = 0, max_v = 0;
        for (unsigned long i = 0; i < nitems; i++) {
            if (states[i] == A_NET_WM_STATE_FULLSCREEN) dominated = 1;
            if (states[i] == A_NET_WM_STATE_HIDDEN) dominated = 1;
            if (states[i] == A_NET_WM_STATE_MAXIMIZED_HORZ) max_h = 1;
            if (states[i] == A_NET_WM_STATE_MAXIMIZED_VERT) max_v = 1;
        }
        if (max_h && max_v) dominated = 1;
        XFree(data);
        if (dominated) {
            if (cfg_debug) printf("  skip: maximized/fullscreen/hidden\n");
            return 0;
        }
    } else if (data) {
        XFree(data);
    }

    return 1;
}

/* ── Client window finder (reparenting WM support) ───────────────── */

static Window find_client_window(Window w) {
    unsigned long nitems = 0;
    unsigned char* data = get_prop(w, A_WM_STATE, A_WM_STATE, NULL, &nitems);
    if (data) {
        XFree(data);
        return w;
    }

    Window dummy, *children = NULL;
    unsigned int nch = 0;
    if (!XQueryTree(dpy, w, &dummy, &dummy, &children, &nch))
        return None;

    Window found = None;
    for (unsigned int i = 0; i < nch && found == None; i++)
        found = find_client_window(children[i]);

    if (children) XFree(children);
    return found;
}

/* ── Shadow list operations ──────────────────────────────────────── */

static ShadowEntry* find_shadow_for_toplevel(Window tl) {
    for (ShadowEntry* e = shadow_list; e; e = e->next)
        if (e->toplevel == tl) return e;
    return NULL;
}

static ShadowEntry* find_shadow_for_client(Window cl) {
    for (ShadowEntry* e = shadow_list; e; e = e->next)
        if (e->client == cl) return e;
    return NULL;
}

static ShadowEntry* find_shadow_for_shadow(Window sw) {
    for (ShadowEntry* e = shadow_list; e; e = e->next)
        if (e->shadow == sw) return e;
    return NULL;
}

static void remove_shadow(ShadowEntry* target) {
    ShadowEntry** pp = &shadow_list;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            if (target->full_shadow) cairo_surface_destroy(target->full_shadow);
            if (target->full_shadow_data) free(target->full_shadow_data);
            if (target->pixmap) XFreePixmap(dpy, target->pixmap);
            XDestroyWindow(dpy, target->shadow);
            free(target);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void add_shadow(Window toplevel, Window client, int x, int y, int w, int h) {
    ShadowEntry* e = calloc(1, sizeof(ShadowEntry));
    if (!e) return;

    e->toplevel = toplevel;
    e->client = client;
    e->x = x;
    e->y = y;
    e->w = w;
    e->h = h;
    e->shadow = create_shadow_window(x, y, w, h);

    /* Map first, then render — ensures WM has reparented before we draw */
    XMapWindow(dpy, e->shadow);
    XSync(dpy, False);

    render_shadow(e, w, h);
    stack_shadow_below(e);

    /* Select StructureNotify on the toplevel for move/resize */
    XSelectInput(dpy, toplevel, StructureNotifyMask | PropertyChangeMask);
    if (client != toplevel)
        XSelectInput(dpy, client, PropertyChangeMask);

    e->next = shadow_list;
    shadow_list = e;
}

/* ── Window checking ─────────────────────────────────────────────── */

static void get_absolute_geometry(Window w, int* x, int* y, int* width, int* height) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) {
        *x = *y = *width = *height = 0;
        return;
    }
    *width = wa.width;
    *height = wa.height;

    /* Translate to root coordinates for reliable positioning */
    Window child;
    XTranslateCoordinates(dpy, w, root, 0, 0, x, y, &child);
}

static void check_window(Window w) {
    if (find_shadow_for_toplevel(w)) return;
    if (find_shadow_for_shadow(w)) return;

    Window client = find_client_window(w);
    if (client == None) return;

    if (cfg_debug) printf("check_window: 0x%lx (client 0x%lx)\n", w, client);

    /*
     * Always select PropertyChangeMask on CSD candidates so we get
     * notified if they change state (e.g. un-maximized). We check the
     * base CSD properties first (motif hints, window type) before
     * selecting events, to avoid monitoring every random window.
     */
    if (!is_csd_window(client)) {
        /*
         * Even though is_csd_window returned false, maybe it's a CSD
         * window that's just maximized. The maximized check is inside
         * is_csd_window, so a maximized CSD window returns false.
         * Select PropertyChangeMask on it anyway so we catch un-maximize.
         * We need a lightweight check: does this window have motif hints
         * requesting no decorations? If so, listen on it.
         */
        unsigned long nitems = 0;
        unsigned char* data = get_prop(client, A_MOTIF_WM_HINTS, A_MOTIF_WM_HINTS, NULL, &nitems);
        if (data && nitems >= 3) {
            MotifWmHints* hints = (MotifWmHints*) data;
            if ((hints->flags & MWM_HINTS_DECORATIONS) &&
                hints->decorations == 0) {
                /* It's CSD but was skipped (maximized/fullscreen) — listen */
                XSelectInput(dpy, client, PropertyChangeMask);
                if (w != client)
                    XSelectInput(dpy, w, StructureNotifyMask);
                if (cfg_debug) printf("  monitoring (currently skipped)\n");
            }
            XFree(data);
        } else if (data) {
            XFree(data);
        }
        return;
    }

    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (wa.map_state != IsViewable) return;

    int ax, ay, aw, ah;
    get_absolute_geometry(w, &ax, &ay, &aw, &ah);

    if (cfg_debug) printf("  -> adding shadow for 0x%lx at %d,%d %dx%d\n",
        w,
        ax,
        ay,
        aw,
        ah);

    add_shadow(w, client, ax, ay, aw, ah);
}

static void scan_existing_windows(void) {
    Window dummy, *children = NULL;
    unsigned int nch = 0;
    XQueryTree(dpy, root, &dummy, &dummy, &children, &nch);
    if (!children) return;

    for (unsigned int i = 0; i < nch; i++)
        check_window(children[i]);

    XFree(children);
}

/* ── Event handlers ──────────────────────────────────────────────── */

static void handle_map(XMapEvent* ev) {
    if (ev->override_redirect) return;
    check_window(ev->window);
}

static void handle_unmap(XUnmapEvent* ev) {
    ShadowEntry* e = find_shadow_for_toplevel(ev->window);
    if (e) XUnmapWindow(dpy, e->shadow);
}

static void handle_destroy(XDestroyWindowEvent* ev) {
    ShadowEntry* e = find_shadow_for_toplevel(ev->window);
    if (!e) e = find_shadow_for_client(ev->window);
    if (e) remove_shadow(e);
}

static void handle_expose(XExposeEvent* ev) {
    /* Redraw shadow content from stored pixmap on Expose */
    ShadowEntry* e = find_shadow_for_shadow(ev->window);
    if (!e || !e->pixmap) return;

    GC gc = XCreateGC(dpy, e->shadow, 0, NULL);
    XCopyArea(dpy, e->pixmap, e->shadow, gc, ev->x, ev->y, ev->width, ev->height, ev->x, ev->y);
    XFreeGC(dpy, gc);
}

static void handle_configure(XConfigureEvent* ev) {
    ShadowEntry* e = find_shadow_for_toplevel(ev->window);
    if (!e) {
        /* Untracked window — maybe just un-maximized? Re-check it. */
        check_window(ev->window);
        return;
    }

    /* Get absolute coordinates via XTranslateCoordinates.
     * This synchronous call implicitly throttles the event loop and fetches the
     * most up-to-date window dimensions, safely compressing resize events. */
    int ax, ay, aw, ah;
    get_absolute_geometry(ev->window, &ax, &ay, &aw, &ah);

    int resized = (aw != e->w || ah != e->h);

    e->x = ax;
    e->y = ay;
    e->w = aw;
    e->h = ah;

    int sx = e->x - cfg_radius + cfg_offset_x;
    int sy = e->y - cfg_radius + cfg_offset_y;
    int sw = e->w + 2 * cfg_radius;
    int sh = e->h + 2 * cfg_radius;

    int actual_x = sx;
    int actual_y = sy;
    int actual_w = sw;
    int actual_h = sh;

    int wx, wy, ww, wh;
    get_workarea(&wx, &wy, &ww, &wh);

    if (actual_x < wx) {
        actual_w -= (wx - actual_x);
        actual_x = wx;
    }
    if (actual_y < wy) {
        actual_h -= (wy - actual_y);
        actual_y = wy;
    }
    if (actual_x + actual_w > wx + ww) actual_w = (wx + ww) - actual_x;
    if (actual_y + actual_h > wy + wh) actual_h = (wy + wh) - actual_y;

    if (actual_w <= 0 || actual_h <= 0) {
        actual_w = 1;
        actual_h = 1;
        actual_x = -100;
        actual_y = -100;
    }

    if (resized || actual_w != e->sw || actual_h != e->sh) {
        XMoveResizeWindow(dpy, e->shadow, actual_x, actual_y, actual_w, actual_h);
        render_shadow(e, e->w, e->h);
    } else {
        XMoveWindow(dpy, e->shadow, actual_x, actual_y);
    }

    /* Do NOT call stack_shadow_below here to avoid spam/unpredictable behavior!
     * Stacking is natively maintained by the WM during moves. */
}

static void handle_reparent(XReparentEvent* ev) {
    if (ev->parent == root) {
        /* New toplevel — will be checked on MapNotify */
    } else {
        /* Window reparented away from root */
        ShadowEntry* e = find_shadow_for_toplevel(ev->window);
        if (e) remove_shadow(e);
    }
}

static void handle_property(XPropertyEvent* ev) {
    if (ev->atom != A_MOTIF_WM_HINTS &&
        ev->atom != A_GTK_FRAME_EXTENTS &&
        ev->atom != A_NET_FRAME_EXTENTS &&
        ev->atom != A_NET_WM_STATE) return;

    /* If WM state changed (maximize/fullscreen), re-evaluate */
    if (ev->atom == A_NET_WM_STATE) {
        ShadowEntry* e = find_shadow_for_client(ev->window);
        if (e) {
            if (!is_csd_window(e->client)) {
                if (cfg_debug) printf("state change: removing shadow for 0x%lx\n", e->toplevel);
                remove_shadow(e);
            }
        } else {
            /* Maybe window was un-maximized, check if it now needs shadow */
            Window dummy, parent = None;
            Window* children = NULL;
            unsigned int nch = 0;
            Window w = ev->window;
            while (1) {
                if (!XQueryTree(dpy, w, &dummy, &parent, &children, &nch))
                    return;
                if (children) XFree(children);
                if (parent == root) break;
                w = parent;
            }
            check_window(w);
        }
        return;
    }

    /* Check if this is a client we track */
    ShadowEntry* e = find_shadow_for_client(ev->window);
    if (e) {
        /* Re-evaluate: maybe decorations were added */
        if (!is_csd_window(e->client)) {
            remove_shadow(e);
            return;
        }
    }

    /* Or maybe a window just became CSD */
    if (!e) {
        /* Find the toplevel for this client */
        Window dummy, parent = None;
        Window* children = NULL;
        unsigned int nch = 0;
        Window w = ev->window;

        /* Walk up to find the toplevel (direct child of root) */
        while (1) {
            if (!XQueryTree(dpy, w, &dummy, &parent, &children, &nch))
                return;
            if (children) XFree(children);
            if (parent == root) break;
            w = parent;
        }
        check_window(w);
    }
}

/* ── Signal handler ──────────────────────────────────────────────── */

static void on_signal(int sig) {
    (void) sig;
    running = 0;
}

/* ── Usage / argument parsing ────────────────────────────────────── */

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Render shadows behind CSD (client-side decorated) windows.\n"
        "\n"
        "Options:\n"
        "  --radius  N     Shadow blur radius in pixels  (default: 60)\n"
        "  --opacity F     Shadow opacity 0.0-1.0        (default: 0.85)\n"
        "  --offset-x N    Horizontal shadow offset       (default: 0)\n"
        "  --offset-y N    Vertical shadow offset          (default: 12)\n"
        "  --color RRGGBB  Shadow color in hex            (default: 000000)\n"
        "  --help          Show this help\n",
        prog);
}

static int parse_hex_color(const char* s) {
    unsigned int r, g, b;
    if (sscanf(s, "%2x%2x%2x", &r, &g, &b) != 3) return -1;
    cfg_color_r = r / 255.0f;
    cfg_color_g = g / 255.0f;
    cfg_color_b = b / 255.0f;
    return 0;
}

static void parse_args(int argc, char** argv) {
    static struct option longopts[] = {
        {"radius", required_argument, NULL, 'r'},
        {"opacity", required_argument, NULL, 'o'},
        {"offset-x", required_argument, NULL, 'x'},
        {"offset-y", required_argument, NULL, 'y'},
        {"color", required_argument, NULL, 'c'},
        {"debug", no_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "r:o:x:y:c:dh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'r': cfg_radius = atoi(optarg); break;
        case 'o': cfg_opacity = atof(optarg); break;
        case 'x': cfg_offset_x = atoi(optarg); break;
        case 'y': cfg_offset_y = atoi(optarg); break;
        case 'c':
            if (parse_hex_color(optarg) < 0) {
                fprintf(stderr, "Invalid color: %s\n", optarg);
                exit(1);
            }
            break;
        case 'd': cfg_debug = 1; break;
        case 'h': usage(argv[0]); exit(0);
        default: usage(argv[0]); exit(1);
        }
    }

    if (cfg_radius < 1) cfg_radius = 1;
    if (cfg_opacity < 0) cfg_opacity = 0;
    if (cfg_opacity > 1) cfg_opacity = 1;
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(int argc, char** argv) {
    parse_args(argc, argv);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }

    XSetErrorHandler(x_error_handler);

    scr = DefaultScreen(dpy);
    screen_w = DisplayWidth(dpy, scr);
    screen_h = DisplayHeight(dpy, scr);
    root = RootWindow(dpy, scr);

    /* Find 32-bit ARGB visual */
    argb_visual = find_argb_visual();
    if (!argb_visual) {
        fprintf(stderr, "No ARGB visual found (is a compositor running?)\n");
        XCloseDisplay(dpy);
        return 1;
    }
    argb_cmap = XCreateColormap(dpy, root, argb_visual, AllocNone);

    /* Intern atoms */
    A_MOTIF_WM_HINTS = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    A_GTK_FRAME_EXTENTS = XInternAtom(dpy, "_GTK_FRAME_EXTENTS", False);
    A_NET_FRAME_EXTENTS = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
    A_NET_WM_WINDOW_TYPE = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    A_NET_WM_WINDOW_TYPE_NORMAL = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    A_NET_WM_WINDOW_TYPE_DIALOG = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    A_WM_STATE = XInternAtom(dpy, "WM_STATE", False);
    A_NET_WM_STATE = XInternAtom(dpy, "_NET_WM_STATE", False);
    A_NET_WM_STATE_MAXIMIZED_VERT = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    A_NET_WM_STATE_MAXIMIZED_HORZ = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    A_NET_WM_STATE_FULLSCREEN = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    A_NET_WM_STATE_HIDDEN = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    A_NET_WM_STATE_SKIP_TASKBAR = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    A_NET_WM_STATE_SKIP_PAGER = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    A_NET_WM_WINDOW_TYPE_UTILITY = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    A_SHADOW_WINDOW = XInternAtom(dpy, "_SHADOW_WINDOW", False);
    A_NET_RESTACK_WINDOW = XInternAtom(dpy, "_NET_RESTACK_WINDOW", False);
    A_NET_ACTIVE_WINDOW = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    A_NET_WORKAREA = XInternAtom(dpy, "_NET_WORKAREA", False);

    /* Initialize workarea cache */
    update_workarea_cache();

    /* Listen for substructure events on root */
    XSelectInput(dpy, root, SubstructureNotifyMask | PropertyChangeMask);

    /* Signal handling for clean exit */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* Scan existing windows */
    scan_existing_windows();

    printf("window-shadow: running (radius=%d, opacity=%.2f, offset=%d,%d)\n",
        cfg_radius,
        cfg_opacity,
        cfg_offset_x,
        cfg_offset_y);

    /* Main event loop */
    XEvent ev;
    while (running) {
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case MapNotify: handle_map(&ev.xmap); break;
        case UnmapNotify: handle_unmap(&ev.xunmap); break;
        case DestroyNotify: handle_destroy(&ev.xdestroywindow); break;
        case ConfigureNotify: handle_configure(&ev.xconfigure); break;
        case ReparentNotify: handle_reparent(&ev.xreparent); break;
        case PropertyNotify:
            handle_property(&ev.xproperty);
            if (ev.xproperty.window == root) {
                if (ev.xproperty.atom == A_NET_WORKAREA) {
                    update_workarea_cache();
                } else if (ev.xproperty.atom == A_NET_ACTIVE_WINDOW) {
                    /* Get the currently active window */
                unsigned long nitems = 0;
                unsigned char* data = get_prop(root, A_NET_ACTIVE_WINDOW, XA_WINDOW, NULL, &nitems);
                Window active = None;
                if (data && nitems > 0) {
                    active = *(Window*) data;
                    XFree(data);
                } else if (data) {
                    XFree(data);
                }
                /* Re-stack the active window's shadow exactly below it.
                 * This keeps shadows properly interleaved without spamming. */
                for (ShadowEntry* e = shadow_list; e; e = e->next) {
                    if (e->toplevel == active || e->client == active) {
                        if (!e->is_active) {
                            e->is_active = 1;
                            render_shadow(e, e->w, e->h);
                            XClearWindow(dpy, e->shadow);
                        }
                        stack_shadow_below(e);
                    } else {
                        if (e->is_active) {
                            e->is_active = 0;
                            render_shadow(e, e->w, e->h);
                            XClearWindow(dpy, e->shadow);
                        }
                    }
                }
                }
            }
            break;
        case Expose: handle_expose(&ev.xexpose); break;
        }
    }

    /* Cleanup */
    printf("window-shadow: exiting\n");
    while (shadow_list)
        remove_shadow(shadow_list);

    XFreeColormap(dpy, argb_cmap);
    XCloseDisplay(dpy);
    return 0;
}
