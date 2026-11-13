/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * status_line.c: an always-visible state line, similar to the Emacs mode line.
 *
 * It shows what the window manager is currently doing: the active binding mode,
 * the workspace and layout of the focused window, whether it is tiled, floating
 * or fullscreen, and the last executed action. The line is an override-redirect
 * window (no focus, click-through when the Shape extension is available) at the
 * top of the focused output.
 *
 */
#include "all.h"

#include <cairo/cairo.h>
#include <xcb/shape.h>

#define SL_PAD logical_px(4)
#define SL_GAP logical_px(12)

static xcb_window_t line_window = XCB_NONE;
static surface_t line_surface;
static bool line_visible = false;
static int line_x = 0;
static int line_y = 0;
static int line_w = 0;
static int line_h = 0;

/* Cached text, so that the line is only redrawn when something changed. */
static char line_text[1024] = "";
static char last_command[512] = "";

static void utf8_truncate_tail(char *str);

void status_line_set_last_command(const char *command) {
    if (command == NULL) {
        last_command[0] = '\0';
        return;
    }

    /* Collapse newlines/tabs so that the line stays a single line. */
    size_t out = 0;
    for (size_t i = 0; command[i] != '\0' && out + 1 < sizeof(last_command); i++) {
        const unsigned char c = (unsigned char)command[i];
        last_command[out++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (char)c;
    }
    last_command[out] = '\0';
    utf8_truncate_tail(last_command);
}

void status_line_hide(void) {
    if (!line_visible || line_window == XCB_NONE) {
        return;
    }

    draw_util_surface_free(conn, &line_surface);
    xcb_destroy_window(conn, line_window);
    line_window = XCB_NONE;
    line_visible = false;
    line_text[0] = '\0';
    xcb_flush(conn);
}

static const char *layout_name(layout_t layout) {
    switch (layout) {
        case L_DEFAULT:
            return "splith";
        case L_SPLITH:
            return "splith";
        case L_SPLITV:
            return "splitv";
        case L_TABBED:
            return "tabbed";
        case L_STACKED:
            return "stacked";
        case L_SCROLLER:
            return "scroller";
        case L_DWINDLE:
            return "dwindle";
        case L_MASTER:
            return "master";
        case L_DOCKAREA:
            return "dockarea";
        case L_OUTPUT:
            return "output";
    }
    return "?";
}

/*
 * Removes an incomplete UTF-8 sequence at the end of the string (we may have
 * truncated a window title in the middle of a multi-byte character).
 *
 */
static void utf8_truncate_tail(char *str) {
    size_t len = strlen(str);
    if (len == 0) {
        return;
    }

    size_t i = len;
    int continuation = 0;
    while (i > 0 && ((unsigned char)str[i - 1] & 0xC0) == 0x80) {
        i--;
        continuation++;
    }
    if (i == 0) {
        str[0] = '\0';
        return;
    }

    const unsigned char lead = (unsigned char)str[i - 1];
    const int needed = (lead >= 0xF0) ? 3 : (lead >= 0xE0) ? 2
                        : (lead >= 0xC0) ? 1
                                         : 0;
    if (needed > continuation) {
        str[i - 1] = '\0';
    }
}

static void build_text(char *buf, size_t len) {
    const char *ws_name = "(no workspace)";
    const char *layout = "?";
    const char *title = "(no focus)";
    const char *state = "";

    Con *workspace = (focused != NULL ? con_get_workspace(focused) : NULL);
    if (workspace != NULL) {
        ws_name = workspace->name;
    }

    /* When focus is on a container (e.g. the workspace or a split container,
     * which happens while a window does not take focus), descend along the
     * focus order to the window the user actually thinks of as focused. */
    Con *target = focused;
    while (target != NULL && target->window == NULL && !TAILQ_EMPTY(&(target->focus_head))) {
        target = TAILQ_FIRST(&(target->focus_head));
    }

    if (target != NULL) {
        if (target->parent != NULL) {
            layout = layout_name(target->parent->layout);
        }
        /* Window containers keep their title in window->name (an i3String);
         * con->name is NULL for them. */
        if (target->window != NULL && target->window->name != NULL) {
            title = i3string_as_utf8(target->window->name);
        } else if (target->name != NULL) {
            title = target->name;
        }
        if (con_is_floating(target)) {
            state = " floating";
        } else if (target->fullscreen_mode != CF_NONE) {
            state = " fullscreen";
        } else {
            state = " tiled";
        }
    }

    char short_title[96];
    snprintf(short_title, sizeof(short_title), "%s", title);
    utf8_truncate_tail(short_title);

    snprintf(buf, len, "ws:%s  layout:%s  %s%s  last:%s",
             ws_name, layout, short_title, state,
             last_command[0] != '\0' ? last_command : "-");
}

void status_line_update(void) {
    if (!config.status_line || conn == NULL || root_screen == NULL || config.font.height == 0) {
        return;
    }

    char text[sizeof(line_text)];
    build_text(text, sizeof(text));

    Rect out = {0, 0, root_screen->width_in_pixels, root_screen->height_in_pixels};
    Output *output = (focused != NULL ? get_output_for_con(focused) : NULL);
    if (output != NULL) {
        out = output->rect;
    }

    const int height = config.font.height + 2 * SL_PAD;
    if (out.width <= 0) {
        return;
    }

    if (line_window == XCB_NONE || line_x != out.x || line_y != out.y ||
        line_w != out.width || line_h != height) {
        if (line_window != XCB_NONE) {
            draw_util_surface_free(conn, &line_surface);
            xcb_destroy_window(conn, line_window);
            line_window = XCB_NONE;
            line_visible = false;
        }

        line_x = out.x;
        line_y = out.y;
        line_w = out.width;
        line_h = height;

        const xcb_visualid_t visual = get_visualid_by_depth(root_depth);
        const uint32_t values[] = {
            root_screen->black_pixel,
            root_screen->black_pixel,
            1,
            colormap,
        };
        const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                              XCB_CW_OVERRIDE_REDIRECT | XCB_CW_COLORMAP;
        const Rect dims = {line_x, line_y, line_w, line_h};
        line_window = create_window(conn, dims, root_depth, visual, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                    XCURSOR_CURSOR_POINTER, false, mask, (uint32_t *)values);

        /* Make the line click-through: an empty input shape means the X server
         * delivers pointer events to the window below. */
        if (shape_supported) {
            xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, 0,
                                 line_window, 0, 0, 0, NULL);
        }

        draw_util_surface_init(conn, &line_surface, line_window, get_visualtype_by_id(visual),
                               line_w, line_h);
        /* Mapping with a background pixel clears the window, so map first. */
        xcb_map_window(conn, line_window);
        line_visible = true;
        line_text[0] = '\0';
    }

    if (strcmp(text, line_text) != 0) {
        const color_t bg = config.client.focused.background;
        const color_t fg = config.client.focused.text;
        const color_t mode_fg = config.client.focused.indicator;

        draw_util_rectangle(&line_surface, bg, 0, 0, line_w, line_h);

        char mode_text[128];
        snprintf(mode_text, sizeof(mode_text), "[%s]",
                 current_binding_mode != NULL ? current_binding_mode : "default");
        i3String *mode = i3string_from_utf8(mode_text);
        const int mode_w = predict_text_width(mode);
        const int text_y = (line_h - config.font.height) / 2;

        draw_util_text(mode, &line_surface, mode_fg, bg, SL_PAD, text_y,
                       line_w - 2 * SL_PAD);
        i3string_free(mode);

        i3String *rest = i3string_from_utf8(text);
        draw_util_text(rest, &line_surface, fg, bg, SL_PAD + mode_w + SL_GAP, text_y,
                       line_w - SL_PAD - mode_w - SL_GAP - SL_PAD);
        i3string_free(rest);

        CAIRO_SURFACE_FLUSH(line_surface.surface);
        snprintf(line_text, sizeof(line_text), "%s", text);
    }

    /* Managed frames are raised by i3 during the render, so raise the line
     * again at the end of each update. */
    const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, line_window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
    xcb_flush(conn);
}
