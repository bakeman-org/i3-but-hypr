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
    Con *con;
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
    int index;
    int count;
    struct switcher_candidate candidates[SW_MAX_CANDIDATES];
    xcb_window_t panel;
    surface_t panel_surface;
    struct switcher_geometry geo;
} sw;

/******************************************************************************
 * Candidate collection
 ******************************************************************************/

static void collect_candidates(Con *con, Con *workspace) {
    if (sw.count >= SW_MAX_CANDIDATES) {
        return;
    }

    if (con_is_leaf(con)) {
        if (con->type != CT_CON || con->window == NULL || !con->mapped) {
            return;
        }
        if (con->window->dock != W_NODOCK) {
            return;
        }
        if (con_get_workspace(con) != workspace) {
            return;
        }

        struct switcher_candidate *c = &(sw.candidates[sw.count++]);
        c->con = con;
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
 * Captures the content of a candidate window as a cairo image surface. Returns
 * NULL if this is not possible.
 *
 * The switcher captures all previews before its panel is mapped, so reading
 * the window content straight from the screen is enough (and, unlike
 * Composite redirection, also works on X servers without a compositing
 * manager or with a dummy/broken Composite implementation, e.g. nested
 * Xephyr).
 *
 */
static cairo_surface_t *capture_preview(Con *con) {
    if (con->window == NULL || !con->mapped) {
        return NULL;
    }

    const xcb_window_t win = con->window->id;
    /* Cairo can only consume the common 24/32-bit visuals. */
    if (con->window->depth < 24) {
        return NULL;
    }
    const int w = (int)con->window_rect.width;
    const int h = (int)con->window_rect.height;
    if (w < 1 || h < 1) {
        return NULL;
    }

    xcb_get_image_cookie_t cookie = xcb_get_image(
        conn, XCB_IMAGE_FORMAT_Z_PIXMAP, win, 0, 0, w, h, ~((uint32_t)0));
    xcb_get_image_reply_t *reply = xcb_get_image_reply(conn, cookie, NULL);
    if (reply == NULL) {
        return NULL;
    }

    /* A depth-24 ZPixmap image contains three 8-bit channels plus one unused
     * byte per pixel which cairo must not interpret as an alpha channel, so
     * RGB24 is the right format there. For depth-32 the bytes are ARGB.
     * X servers use 32 bits per pixel for both depths. */
    const cairo_format_t format = (reply->depth == 32) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
    cairo_surface_t *image = cairo_image_surface_create_for_data(
        xcb_get_image_data(reply), format, w, h, w * 4);

    /* The image data is owned by the reply; keep both alive until the surface
     * is destroyed. */
    cairo_surface_set_user_data(image, (const cairo_user_data_key_t *)&format,
                                reply, (cairo_destroy_func_t)free);

    return image;
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

    g->title_h = font_h + SW_TITLE_GAP;

    const int max_panel_w = (int)(out_rect->width * 0.8);
    g->per_row = (max_panel_w + SW_GAP) / (g->tile_w + SW_GAP);
    if (g->per_row < 1) {
        g->per_row = 1;
    }
    const int rows = (sw.count + g->per_row - 1) / g->per_row;

    g->panel_w = g->per_row * g->tile_w + (g->per_row - 1) * SW_GAP + 2 * SW_PAD;
    g->panel_h = rows * (g->tile_h + g->title_h) + (rows - 1) * SW_GAP + 2 * SW_PAD;

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
        i3String *title = i3string_from_utf8(c->con->name ? c->con->name : "");
        draw_util_text(title, surface, config.client.unfocused.text,
                       config.client.unfocused.background,
                       x + logical_px(8), y + h / 2 - config.font.height / 2, w - logical_px(16));
        i3string_free(title);
    }

    cairo_restore(cr);
}

static void draw_tile(int i) {
    struct switcher_geometry *g = &(sw.geo);
    surface_t *surface = &(sw.panel_surface);
    cairo_t *cr = surface->cr;

    const int row = i / g->per_row;
    const int col = i % g->per_row;
    const int x = SW_PAD + col * (g->tile_w + SW_GAP);
    const int y = SW_PAD + row * (g->tile_h + g->title_h + SW_GAP);

    cairo_save(cr);

    /* Border frame: highlighted for the selected tile. */
    const struct Colortriple *colors = (i == sw.index) ? &(config.client.focused)
                                                       : &(config.client.unfocused);
    draw_util_rectangle(surface, colors->border, x, y, g->tile_w, g->tile_h + g->title_h);

    draw_preview(&(sw.candidates[i]), x + 1, y + 1, g->tile_w - 2, g->tile_h - 2);

    /* Title below the preview. */
    struct switcher_candidate *c = &(sw.candidates[i]);
    i3String *title = i3string_from_utf8(c->con->name ? c->con->name : "");
    draw_util_text(title, surface, colors->text, config.client.focused.background,
                   x, y + g->tile_h + (g->title_h - config.font.height) / 2, g->tile_w);
    i3string_free(title);

    cairo_restore(cr);
}

static void draw_panel(void) {
    if (!sw.active) {
        return;
    }

    surface_t *surface = &(sw.panel_surface);

    draw_util_rectangle(surface, config.client.focused.background, 0, 0, sw.geo.panel_w, sw.geo.panel_h);

    for (int i = 0; i < sw.count; i++) {
        draw_tile(i);
    }

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
    }
    sw.count = 0;
}

static void open_panel(int direction) {
    memset(&sw, 0, sizeof(sw));

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
    draw_panel();
    xcb_flush(conn);
}

void switcher_close(void) {
    if (!sw.active) {
        return;
    }
    sw.active = false;

    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);

    draw_util_surface_free(conn, &(sw.panel_surface));
    xcb_destroy_window(conn, sw.panel);
    sw.panel = XCB_NONE;
    free_candidates();
    xcb_flush(conn);
}

void switcher_commit(void) {
    if (!sw.active) {
        return;
    }

    Con *con = sw.candidates[sw.index].con;
    switcher_close();

    if (con == NULL || con->window == NULL || !con->mapped) {
        /* The candidate vanished while the switcher was open. */
        return;
    }

    con_focus(con);
    tree_render();
}

/******************************************************************************
 * Public API
 ******************************************************************************/

void switcher_next(int direction) {
    if (!sw.active) {
        open_panel(direction);
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
        /* mangowc semantics: releasing any modifier confirms the selection. */
        if (is_modifier_keysym(sym)) {
            switcher_commit();
            return true;
        }
        /* Releases of regular keys are only auto-repeat artifacts. */
        return true;
    }

    if (is_cycle_keysym(sym)) {
        const int direction = (event->state & XCB_MOD_MASK_SHIFT) ? -1 : 1;
        switcher_next(direction);
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

    /* Any other key cancels the switcher and is swallowed. */
    switcher_close();
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

    const int tile_total_h = g->tile_h + g->title_h;
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
