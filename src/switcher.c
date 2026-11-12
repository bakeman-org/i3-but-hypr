/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * switcher.c: niri-style alt-tab window switcher with thumbnail previews,
 *             ported from mangowc's switcher (src/switcher/switcher.c).
 *
 * While the switcher is open, the keyboard is grabbed so that we can see
 * modifier releases (which confirm the selection) and auto-repeated Tab
 * presses (which cycle the selection) without relying on user bindings.
 * Window previews are captured through the Composite extension once when the
 * switcher opens; windows for which no preview can be captured fall back to
 * a title-only tile.
 *
 */
#include "all.h"

#include <cairo.h>
#include <math.h>
#include <xkbcommon/xkbcommon.h>

/* Layout state of the open switcher. */
struct switcher_candidate {
    /* Only valid between collect_candidates() and the end of open_panel(); the
     * drawing and commit paths use the window id instead. */
    Con *con;
    /* X11 window id of the candidate. The Con pointer is only used while the
     * switcher is being drawn; when committing, the window is looked up again
     * by id because the container may have been closed in the meantime. */
    xcb_window_t id;
    /* Captured preview (NULL when the window could not be captured). */
    cairo_surface_t *image;
    int img_w, img_h;
};

struct switcher_geometry {
    int tile_w, tile_h;
    int title_h;
    int per_row;
    int panel_w, panel_h;
    int panel_x, panel_y;
};

static struct {
    bool active;
    /* Sticky mode: modifier releases do not confirm the selection, so the
     * panel can be examined (and screenshotted) at leisure. */
    bool sticky;
    int index;
    int count;
    struct switcher_candidate candidates[SW_MAX_CANDIDATES];
    xcb_window_t panel;
    surface_t panel_surface;
    struct switcher_geometry geo;
    /* Auto-repeat bookkeeping for the cycle key, see switcher_handle_key(). */
    bool cycle_key_down;
    xcb_timestamp_t cycle_release_time;
} sw;

/* Polls whether the modifiers are still held while the switcher is open, see
 * switcher_check_modifiers(). */
static ev_timer modifier_timer;

static void switcher_check_modifiers(void);
static void modifier_tick_cb(EV_P_ ev_timer *w, int revents);

/* How often the modifier state is polled while the switcher is open. */
#define SW_MODIFIER_POLL 0.05

/******************************************************************************
 * Candidate collection
 ******************************************************************************/

static void collect_candidates(Con *con, Con *workspace) {
    if (sw.count >= SW_MAX_CANDIDATES) {
        return;
    }

    if (con_is_leaf(con)) {
        if (con->type != CT_CON || con->window == NULL) {
            return;
        }
        if (con->window->dock != W_NODOCK) {
            return;
        }
        if (con_get_workspace(con) != workspace) {
            return;
        }

        /* Note that con->mapped is deliberately not required: windows that are
         * currently hidden inside a tabbed/stacked container are unmapped and
         * must still be reachable with the switcher (that is the whole point of
         * an alt-tab list). They simply have no preview, see
         * capture_preview(). */
        struct switcher_candidate *c = &(sw.candidates[sw.count++]);
        c->con = con;
        c->id = con->window->id;
        c->image = NULL;
        c->img_w = 0;
        c->img_h = 0;
        return;
    }

    /* focus_head is most-recently-focused first (see con_focus()), so walking
     * it in order yields the global MRU order of leaves. */
    Con *child;
    TAILQ_FOREACH (child, &(con->focus_head), focused) {
        collect_candidates(child, workspace);
    }
}

/*
 * Grabs a rectangle of the given drawable and wraps the reply in a cairo image
 * surface. Returns NULL if that is not possible.
 *
 * `has_alpha` says whether the pixel data carries a meaningful alpha channel
 * (only the case for depth-32 client windows). Snapshots of the screen are
 * always treated as opaque: their fourth byte is unused and would otherwise
 * make the whole preview transparent, i.e. look black.
 *
 */
static cairo_surface_t *capture_area(xcb_drawable_t drawable, int x, int y, int w, int h, bool has_alpha) {
    if (w < 1 || h < 1) {
        return NULL;
    }

    xcb_get_image_cookie_t cookie = xcb_get_image(
        conn, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable, x, y, w, h, ~((uint32_t)0));
    xcb_get_image_reply_t *reply = xcb_get_image_reply(conn, cookie, NULL);
    if (reply == NULL) {
        return NULL;
    }

    /* A depth-24 ZPixmap image contains three 8-bit channels plus one unused
     * byte per pixel which cairo must not interpret as an alpha channel, so
     * RGB24 is the right format there. For depth-32 the bytes are ARGB.
     * X servers use 32 bits per pixel for both depths. */
    const cairo_format_t format = (has_alpha && reply->depth == 32) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
    cairo_surface_t *image = cairo_image_surface_create_for_data(
        xcb_get_image_data(reply), format, w, h, w * 4);
    if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(image);
        free(reply);
        return NULL;
    }

    /* The image data is owned by the reply; keep both alive until the surface
     * is destroyed. */
    static const cairo_user_data_key_t image_data_key;
    cairo_surface_set_user_data(image, &image_data_key, reply, (cairo_destroy_func_t)free);

    return image;
}

/*
 * Whether a captured surface contains nothing but black/transparent pixels.
 *
 * A capture can succeed and still be useless: without a compositing manager the
 * content of an obscured window is undefined (in practice black), and depth-32
 * client windows may report an alpha value of zero. A coarse sample of the
 * image is enough to detect that, and it is much cheaper than scanning it.
 *
 */
static bool image_is_blank(cairo_surface_t *image) {
    const int w = cairo_image_surface_get_width(image);
    const int h = cairo_image_surface_get_height(image);
    const int stride = cairo_image_surface_get_stride(image);
    const unsigned char *data = cairo_image_surface_get_data(image);
    if (data == NULL || w < 9 || h < 9) {
        return false;
    }

    const bool has_alpha = (cairo_image_surface_get_format(image) == CAIRO_FORMAT_ARGB32);
    for (int gy = 1; gy <= 8; gy++) {
        for (int gx = 1; gx <= 8; gx++) {
            uint32_t pixel;
            memcpy(&pixel, data + (size_t)(h * gy / 9) * stride + (size_t)(w * gx / 9) * 4, sizeof(pixel));
            /* Fully transparent pixels carry no information. */
            if (has_alpha && (pixel >> 24) == 0) {
                continue;
            }
            if ((pixel & 0x00ffffff) != 0) {
                return false;
            }
        }
    }

    return true;
}

/*
 * Captures the content of a candidate window as a cairo image surface. Returns
 * NULL if this is not possible.
 *
 * The switcher captures all previews before its panel is mapped, so reading the
 * window content straight from the screen is enough (and, unlike Composite
 * redirection, also works on X servers without a compositing manager or with a
 * dummy/broken Composite implementation, e.g. nested Xephyr).
 *
 * Two fallbacks keep previews from turning into black tiles:
 *  - Windows that are not mapped at all (hidden tab in a tabbed/stacked
 *    container) cannot be captured; they get a title-only tile.
 *  - A capture that came out blank (obscured window without a compositor, or an
 *    alpha-less depth-32 window) is retried as a snapshot of the screen area
 *    covered by the window's frame, which always contains real pixels.
 *
 */
static cairo_surface_t *capture_preview(Con *con) {
    if (con->window == NULL || !con->mapped) {
        return NULL;
    }

    cairo_surface_t *image = NULL;
    /* Cairo can only consume the common 24/32-bit visuals. */
    if (con->window->depth >= 24) {
        image = capture_area(con->window->id, 0, 0,
                             (int)con->window_rect.width, (int)con->window_rect.height,
                             con->window->depth == 32);
        if (image != NULL && !image_is_blank(image)) {
            return image;
        }
    }

    /* Fall back to a snapshot of the window's frame (decoration included),
     * clamped to the root window since GetImage rejects rects outside of its
     * drawable. */
    int x = con->rect.x;
    int y = con->rect.y;
    int w = (int)con->rect.width;
    int h = (int)con->rect.height;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > (int)croot->rect.width) {
        w = (int)croot->rect.width - x;
    }
    if (y + h > (int)croot->rect.height) {
        h = (int)croot->rect.height - y;
    }

    cairo_surface_t *snapshot = capture_area(root, x, y, w, h, false);
    if (snapshot == NULL) {
        /* Keep the (blank) direct capture as a last resort rather than showing
         * the title-only tile. */
        return image;
    }

    if (image != NULL) {
        cairo_surface_destroy(image);
    }
    return snapshot;
}

/*
 * Title of a candidate window. Con containers for X windows keep the name in
 * con->window->name (con->name is NULL for them).
 *
 */
static const char *candidate_title(struct switcher_candidate *c) {
    /* Resolve the container by window id instead of using c->con: the window
     * may have been closed while the switcher was open, and a dangling Con
     * pointer would crash the window manager. */
    Con *con = con_by_window_id(c->id);
    if (con == NULL) {
        return "";
    }
    if (con->window != NULL && con->window->name != NULL) {
        return i3string_as_utf8(con->window->name);
    }
    return con->name != NULL ? con->name : "";
}

/******************************************************************************
 * Geometry & drawing
 ******************************************************************************/

static void compute_geometry(void) {
    Con *ws = con_get_workspace(focused);
    const Rect *out_rect = (ws != NULL) ? &(ws->rect) : &(focused->rect);

    struct switcher_geometry *g = &(sw.geo);
    const int font_h = config.font.height;

    /* Tile size: fixed base height, clamped to a fraction of the workspace. */
    g->tile_h = logical_px(160);
    const int max_h = (int)(out_rect->height * 0.22);
    if (g->tile_h > max_h) {
        g->tile_h = MAX(max_h, logical_px(80));
    }
    g->tile_w = (int)(g->tile_h * 16.0 / 9.0);
    const int max_w = (int)(out_rect->width * 0.8 - 2 * SW_MARGIN);
    if (g->tile_w > max_w) {
        g->tile_w = MAX(max_w, logical_px(120));
    }

    /* One shared title bar below the tiles shows the title of the currently
     * selected window (mangowc's title_message_bar). */
    g->title_h = font_h + 2 * SW_TITLE_GAP;

    const int max_panel_w = (int)(out_rect->width * 0.8);
    g->per_row = (max_panel_w + SW_GAP) / (g->tile_w + SW_GAP);
    if (g->per_row < 1) {
        g->per_row = 1;
    }
    /* Do not reserve columns for tiles that do not exist. */
    if (g->per_row > sw.count) {
        g->per_row = sw.count;
    }
    const int rows = (sw.count + g->per_row - 1) / g->per_row;

    g->panel_w = g->per_row * g->tile_w + (g->per_row - 1) * SW_GAP + 2 * SW_PAD;
    g->panel_h = rows * g->tile_h + (rows - 1) * SW_GAP + 2 * SW_PAD + SW_TITLE_GAP + g->title_h;

    g->panel_x = out_rect->x + ((int)out_rect->width - g->panel_w) / 2;
    g->panel_y = out_rect->y + ((int)out_rect->height - g->panel_h) / 2;
}

static void draw_preview(struct switcher_candidate *c, int x, int y, int w, int h) {
    surface_t *surface = &(sw.panel_surface);
    cairo_t *cr = surface->cr;
    cairo_save(cr);

    /* Fill the preview area with the tile background first. */
    draw_util_rectangle(surface, config.client.unfocused.background, x, y, w, h);

    if (c->image != NULL) {
        const int iw = c->img_w > 0 ? c->img_w : cairo_image_surface_get_width(c->image);
        const int ih = c->img_h > 0 ? c->img_h : cairo_image_surface_get_height(c->image);
        const double scale = MAX((double)w / iw, (double)h / ih);
        const double dw = iw * scale;
        const double dh = ih * scale;

        cairo_rectangle(cr, x, y, w, h);
        cairo_clip(cr);
        cairo_translate(cr, x + (w - dw) / 2, y + (h - dh) / 2);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, c->image, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    } else {
        /* Fallback: a large, dimmed window title inside the tile. */
        i3String *title = i3string_from_utf8(candidate_title(c));
        draw_util_text(title, surface, config.client.unfocused.text,
                       config.client.unfocused.background,
                       x + logical_px(8), y + h / 2 - config.font.height / 2, w - logical_px(16));
        i3string_free(title);
    }

    cairo_restore(cr);
}

/*
 * Draws the frame (SW_PAD wide border ring) around a tile, mirroring mangowc:
 * the selected tile uses the focused colors, the others the unfocused ones.
 * Gradient borders are honored so that the switcher matches the window
 * borders of the running config.
 *
 */
static void draw_tile_frame(bool selected, int x, int y, int w, int h) {
    surface_t *surface = &(sw.panel_surface);
    const struct Colortriple *colors = selected ? &(config.client.focused)
                                                : &(config.client.unfocused);
    const struct gradient_spec *gradient = selected ? &(config.client_gradients.focused)
                                                    : &(config.client_gradients.unfocused);

    if (gradient->enabled) {
        draw_util_gradient_rounded_rectangle(surface, gradient->start, gradient->end,
                                             gradient->angle, x, y, w, h, 0);
    } else {
        draw_util_rectangle(surface, colors->border, x, y, w, h);
    }
}

static void draw_tile(int i) {
    struct switcher_geometry *g = &(sw.geo);
    cairo_t *cr = sw.panel_surface.cr;

    const int row = i / g->per_row;
    const int col = i % g->per_row;
    const int x = SW_PAD + col * (g->tile_w + SW_GAP);
    const int y = SW_PAD + row * (g->tile_h + SW_GAP);

    cairo_save(cr);

    draw_tile_frame(i == sw.index, x, y, g->tile_w, g->tile_h);

    /* The preview sits inside the frame, like the client area of a window. */
    draw_preview(&(sw.candidates[i]), x + SW_PAD, y + SW_PAD,
                 g->tile_w - 2 * SW_PAD, g->tile_h - 2 * SW_PAD);

    cairo_restore(cr);
}

static void draw_panel(void) {
    if (!sw.active) {
        return;
    }

    surface_t *surface = &(sw.panel_surface);

    draw_util_rectangle(surface, config.client.background, 0, 0, sw.geo.panel_w, sw.geo.panel_h);

    for (int i = 0; i < sw.count; i++) {
        draw_tile(i);
    }

    /* Shared title bar below the tiles: shows the title of the currently
     * selected window (mangowc's title_message_bar). */
    struct switcher_candidate *sel = &(sw.candidates[sw.index]);
    const struct Colortriple *colors = &(config.client.focused);
    const int bar_y = sw.geo.panel_h - sw.geo.title_h;
    draw_util_rectangle(surface, colors->border, 0, bar_y, sw.geo.panel_w, sw.geo.title_h);
    i3String *sel_title = i3string_from_utf8(candidate_title(sel));
    const int text_w = predict_text_width(sel_title);
    int text_x = (sw.geo.panel_w - text_w) / 2;
    if (text_x < logical_px(8)) {
        text_x = logical_px(8);
    }
    draw_util_text(sel_title, surface, colors->text, colors->border,
                   text_x, bar_y + (sw.geo.title_h - config.font.height) / 2,
                   sw.geo.panel_w - text_x - logical_px(8));
    i3string_free(sel_title);

    /* The panel is an override-redirect window, so i3 does not manage its
     * stacking: the frames of the managed windows would cover it. Raise it
     * whenever it is redrawn. */
    const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, sw.panel, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);

    const cairo_status_t status = cairo_status(surface->cr);
    if (status != CAIRO_STATUS_SUCCESS) {
        ELOG("switcher: cairo context is in an error state: %s\n", cairo_status_to_string(status));
    }

    CAIRO_SURFACE_FLUSH(surface->surface);
    xcb_flush(conn);
}

/******************************************************************************
 * Panel lifecycle
 ******************************************************************************/

static void free_candidates(void) {
    for (int i = 0; i < sw.count; i++) {
        if (sw.candidates[i].image != NULL) {
            cairo_surface_destroy(sw.candidates[i].image);
            sw.candidates[i].image = NULL;
        }
        sw.candidates[i].con = NULL;
        sw.candidates[i].id = XCB_NONE;
    }
    sw.count = 0;
}

static void open_panel(int direction, bool sticky) {
    memset(&sw, 0, sizeof(sw));
    sw.sticky = sticky;

    Con *workspace = con_get_workspace(focused);
    if (workspace == NULL) {
        return;
    }
    collect_candidates(croot, workspace);

    if (sw.count == 0) {
        return;
    }

    compute_geometry();

    /* The panel has to be created with the same depth/visual that cairo draws
     * into, i.e. i3's root_depth (usually 32 bit ARGB, see main.c) — a window
     * with a mismatching depth silently discards all drawing. This mirrors
     * x_con_init(). */
    const xcb_visualid_t visual = get_visualid_by_depth(root_depth);
    const uint32_t values[] = {
        root_screen->black_pixel,
        root_screen->black_pixel,
        1,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS,
        colormap,
    };
    const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                          XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    const Rect dims = {sw.geo.panel_x, sw.geo.panel_y, sw.geo.panel_w, sw.geo.panel_h};

    sw.panel = create_window(conn, dims, root_depth, visual, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                             XCURSOR_CURSOR_POINTER, false, mask, (uint32_t *)values);

    draw_util_surface_init(conn, &(sw.panel_surface), sw.panel, get_visualtype_by_id(visual),
                           sw.geo.panel_w, sw.geo.panel_h);

    /* Capture the previews before mapping the panel so that the tiles show
     * real content right away. */
    for (int i = 0; i < sw.count; i++) {
        sw.candidates[i].image = capture_preview(sw.candidates[i].con);
        if (sw.candidates[i].image != NULL) {
            sw.candidates[i].img_w = cairo_image_surface_get_width(sw.candidates[i].image);
            sw.candidates[i].img_h = cairo_image_surface_get_height(sw.candidates[i].image);
        }
    }

    /* Take control of the keyboard: this delivers auto-repeated Tab presses
     * and, crucially, modifier releases to us even though they are not bound
     * in the user's configuration. */
    xcb_grab_keyboard_cookie_t gcookie =
        xcb_grab_keyboard(conn, 0, root_screen->root, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    xcb_grab_keyboard_reply_t *greply = xcb_grab_keyboard_reply(conn, gcookie, NULL);
    const bool grabbed = (greply != NULL && greply->status == XCB_GRAB_STATUS_SUCCESS);
    free(greply);
    if (!grabbed) {
        ELOG("Could not grab the keyboard, switcher will commit on Enter only\n");
    }

    /* mangowc semantics: tiles[0] is the currently focused window; the
     * selection starts on its neighbor so that committing inserts it at the
     * head of the MRU order and the next invocation toggles back. */
    sw.index = (sw.count > 1) ? (direction < 0 ? sw.count - 1 : 1) : 0;

    xcb_map_window(conn, sw.panel);
    sw.active = true;
    /* The panel is normally opened by the cycle key itself, so that key is held
     * down right now — needed to filter out its auto-repeat, see
     * switcher_handle_key(). */
    sw.cycle_key_down = true;
    draw_panel();
    xcb_flush(conn);

    /* Watch for the modifiers being released. When Alt+Tab is tapped quickly the
     * release is delivered before the keyboard grab above took effect, so it
     * never reaches us and the switcher would stay open without ever committing
     * — the user sees "alt+tab did nothing". */
    ev_timer_stop(main_loop, &modifier_timer);
    ev_timer_init(&modifier_timer, modifier_tick_cb, SW_MODIFIER_POLL, SW_MODIFIER_POLL);
    ev_timer_start(main_loop, &modifier_timer);

    /* The modifier may already be released at this point (very fast tap), so
     * check once right away instead of waiting for the first timer tick. */
    switcher_check_modifiers();
}

void switcher_close(void) {
    if (!sw.active) {
        return;
    }
    sw.active = false;

    ev_timer_stop(main_loop, &modifier_timer);

    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);

    draw_util_surface_free(conn, &(sw.panel_surface));

    /* Destroying the panel makes the pointer end up in whichever window is now
     * below it, so X sends an EnterNotify for that window. With
     * focus_follows_mouse (i3's default) i3 would then focus that window and
     * silently undo the selection we are about to commit: alt+tab would appear
     * to "sometimes" stay on the window the user wanted to leave, depending on
     * where the mouse pointer happens to rest.
     *
     * Therefore ignore the EnterNotify belonging to the DestroyWindow request,
     * just like handle_unmap_notify_event() does for regular windows. The extra
     * GetInputFocus round trip guarantees that later, genuine EnterNotify events
     * carry a different sequence number (see i3 ticket #609). */
    xcb_get_input_focus_cookie_t focus_cookie = xcb_get_input_focus(conn);
    xcb_void_cookie_t destroy_cookie = xcb_destroy_window(conn, sw.panel);
    add_ignore_event(ENSURE_FULL_SEQUENCE(destroy_cookie.sequence), XCB_ENTER_NOTIFY);
    free(xcb_get_input_focus_reply(conn, focus_cookie, NULL));

    sw.panel = XCB_NONE;
    free_candidates();
    xcb_flush(conn);
}

/*
 * Whether the given modifier is currently held down, according to a keymap
 * query.
 *
 */
static bool modifier_is_held(xcb_query_keymap_reply_t *reply, xcb_keysym_t sym) {
    xcb_keycode_t *codes = xcb_key_symbols_get_keycode(keysyms, sym);
    if (codes == NULL) {
        return false;
    }

    bool held = false;
    for (xcb_keycode_t *code = codes; *code != XCB_NO_SYMBOL; code++) {
        if ((size_t)(*code / 8) < sizeof(reply->keys) &&
            (reply->keys[*code / 8] & (1 << (*code % 8)))) {
            held = true;
            break;
        }
    }
    free(codes);

    return held;
}

/*
 * Commits the selection as soon as no "commit" modifier is held anymore.
 *
 * Normally the modifier release is delivered to the keyboard grab and commits
 * right away (see switcher_handle_key()). That event is lost, however, when it
 * is generated before the grab takes effect — which happens when Alt+Tab is
 * tapped quickly, and also when another client holds a keyboard grab. Polling
 * the modifier state makes the commit independent of receiving that event, so
 * the switcher can never get stuck without focusing anything.
 *
 */
static void switcher_check_modifiers(void) {
    if (!sw.active || sw.sticky || keysyms == NULL) {
        return;
    }

    xcb_query_keymap_reply_t *reply = xcb_query_keymap_reply(conn, xcb_query_keymap(conn), NULL);
    if (reply == NULL) {
        return;
    }

    const bool held =
        modifier_is_held(reply, XKB_KEY_Alt_L) || modifier_is_held(reply, XKB_KEY_Alt_R) ||
        modifier_is_held(reply, XKB_KEY_Meta_L) || modifier_is_held(reply, XKB_KEY_Meta_R) ||
        modifier_is_held(reply, XKB_KEY_Super_L) || modifier_is_held(reply, XKB_KEY_Super_R) ||
        modifier_is_held(reply, XKB_KEY_Control_L) || modifier_is_held(reply, XKB_KEY_Control_R);

    free(reply);

    if (!held) {
        DLOG("switcher: modifiers are no longer held, committing\n");
        switcher_commit();
    }
}

static void modifier_tick_cb(EV_P_ ev_timer *w, int revents) {
    switcher_check_modifiers();
}

void switcher_commit(void) {
    if (!sw.active) {
        return;
    }

    const xcb_window_t id = sw.candidates[sw.index].id;
    switcher_close();

    /* Look the window up again: the container may have been closed while the
     * switcher was open, in which case the Con pointer we collected is stale. */
    Con *con = con_by_window_id(id);
    if (con == NULL) {
        /* The candidate vanished while the switcher was open. */
        return;
    }

    /* While the switcher owned the keyboard the X11 input focus could have been
     * moved by someone else (a client calling XSetInputFocus, a revert after the
     * grab, …), so i3's cached focused_id is not trustworthy. Reset it to force
     * x_push_changes() to actually issue XSetInputFocus for the new window. */
    focused_id = XCB_NONE;

    /* con_activate() is con_focus() plus raising the window, which matters for
     * floating candidates. */
    con_activate(con);
    tree_render();
}

/******************************************************************************
 * Public API
 ******************************************************************************/

void switcher_next(int direction, bool sticky) {
    if (!sw.active) {
        open_panel(direction, sticky);
        return;
    }

    if (sw.count < 1) {
        switcher_close();
        return;
    }

    sw.index = (sw.index + direction + sw.count) % sw.count;
    draw_panel();
}

bool switcher_is_active(void) {
    return sw.active;
}

bool switcher_is_panel(xcb_window_t window) {
    return sw.active && window != XCB_NONE && window == sw.panel;
}

/*
 * Modifiers which confirm the selection when they are released (mangowc
 * semantics). Shift is deliberately not in this list: it is the "cycle
 * backwards" modifier, so it must be possible to add/remove it while the
 * switcher is open without prematurely committing.
 *
 */
static bool is_commit_modifier_keysym(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_Alt_L:
        case XKB_KEY_Alt_R:
        case XKB_KEY_Meta_L:
        case XKB_KEY_Meta_R:
        case XKB_KEY_Super_L:
        case XKB_KEY_Super_R:
        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R:
            return true;
        default:
            return false;
    }
}

static bool is_modifier_keysym(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_Alt_L:
        case XKB_KEY_Alt_R:
        case XKB_KEY_Meta_L:
        case XKB_KEY_Meta_R:
        case XKB_KEY_Super_L:
        case XKB_KEY_Super_R:
        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R:
        case XKB_KEY_Shift_L:
        case XKB_KEY_Shift_R:
            return true;
        default:
            return false;
    }
}

static bool is_cycle_keysym(xkb_keysym_t sym) {
    return sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab || sym == XKB_KEY_KP_Tab;
}

bool switcher_handle_key(xcb_key_press_event_t *event) {
    if (!sw.active || keysyms == NULL) {
        return false;
    }

    const bool release = (event->response_type == XCB_KEY_RELEASE);
    const xkb_keysym_t sym = xcb_key_symbols_get_keysym(keysyms, event->detail, 0);

    if (release) {
        /* mangowc semantics: releasing a modifier confirms the selection.
         * In sticky mode releases are ignored so that the panel stays open. */
        if (is_commit_modifier_keysym(sym)) {
            if (!sw.sticky) {
                switcher_commit();
            }
            return true;
        }
        if (is_cycle_keysym(sym)) {
            /* Remember when the release happened instead of clearing the key
             * state right away: legacy auto-repeat sends a release/press pair
             * with an identical timestamp per repeat, so the release may be
             * synthetic. See the press handling below. */
            sw.cycle_release_time = event->time;
            return true;
        }
        /* Releases of other regular keys are only auto-repeat artifacts. */
        return true;
    }

    if (is_cycle_keysym(sym)) {
        /* Ignore auto-repeat: one physical key press advances the selection by
         * exactly one tile. Otherwise holding Tab for a moment runs the
         * selection past the tile the user is looking at — with three windows,
         * cycling on from the previous one lands right back on the window that
         * should have been left, which made alt+tab feel unreliable.
         *
         * Two auto-repeat flavours have to be caught:
         *  - detectable auto-repeat (XKB) sends repeated presses without any
         *    release in between -> the key is still marked as down;
         *  - legacy auto-repeat sends release/press pairs sharing the same
         *    timestamp -> identified by comparing against the release time.
         */
        if (sw.cycle_release_time != XCB_CURRENT_TIME) {
            const bool synthetic_release = (event->time == sw.cycle_release_time);
            sw.cycle_release_time = XCB_CURRENT_TIME;
            if (!synthetic_release) {
                /* The key was really released before this press. */
                sw.cycle_key_down = false;
            }
        }

        if (sw.cycle_key_down) {
            return true;
        }

        const int direction = (event->state & XCB_MOD_MASK_SHIFT) ? -1 : 1;
        switcher_next(direction, sw.sticky);

        /* Set after the call: opening the panel resets the whole state. */
        sw.cycle_key_down = true;
        return true;
    }

    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        switcher_commit();
        return true;
    }

    if (sym == XKB_KEY_Escape) {
        switcher_close();
        return true;
    }

    /* Pressing a modifier must not cancel the switcher: e.g. pressing Shift
     * after Tab (to cycle backwards) or Alt a second time. It is only
     * swallowed. */
    if (is_modifier_keysym(sym)) {
        return true;
    }

    /* Any other key cancels the switcher and is swallowed. In sticky mode it
     * is swallowed without closing so that stray keys do not end the probe. */
    if (!sw.sticky) {
        switcher_close();
    }
    return true;
}

bool switcher_handle_button(xcb_button_press_event_t *event) {
    if (!sw.active || event->response_type != XCB_BUTTON_PRESS) {
        return false;
    }

    if (event->event != sw.panel) {
        return false;
    }

    struct switcher_geometry *g = &(sw.geo);
    const int x = event->event_x - g->panel_x;
    const int y = event->event_y - g->panel_y;

    const int tile_total_h = g->tile_h;
    const int col = (x - SW_PAD + SW_GAP) / (g->tile_w + SW_GAP);
    const int row = (y - SW_PAD + SW_GAP) / (tile_total_h + SW_GAP);
    if (col < 0 || col >= g->per_row) {
        return true;
    }
    const int i = row * g->per_row + col;
    if (i >= 0 && i < sw.count) {
        sw.index = i;
        switcher_commit();
    }
    return true;
}

void switcher_handle_expose(xcb_window_t window) {
    if (sw.active && window == sw.panel) {
        draw_panel();
    }
}
