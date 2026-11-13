/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * i3-exit-dialog.c: tiny native confirmation dialog for `exit i3`.
 *
 * It is a plain xcb + cairo + pango program (no Qt, no GTK, no Python): an
 * override-redirect, keyboard-grabbing card centered on the focused screen.
 * Enter confirms, Escape/q cancel; clicking Cancel/Exit works as well.
 *
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cairo/cairo-xcb.h>
#include <pango/pangocairo.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_keysyms.h>

#define CARD_W 380
#define CARD_H 196
#define CARD_RADIUS 14

#define TITLE_Y 26
#define MESSAGE_Y 64
#define BUTTON_Y 112
#define HINT_Y 166

#define BTN_HEIGHT 36
#define BTN_EXIT_W 112
#define BTN_CANCEL_W 96
#define BTN_GAP 10
#define PAD_X 30

/* Keysyms we need (avoids depending on the X11 keysym headers). */
#define KS_ESCAPE 0xff1b
#define KS_RETURN 0xff0d
#define KS_KP_ENTER 0xff8d
#define KS_Q 0x0071
#define KS_Q_UPPER 0x0051

typedef struct {
    double r;
    double g;
    double b;
} color_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} rect_t;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_window_t win;
static xcb_window_t previous_focus = XCB_NONE;
static cairo_surface_t *surface;

static double scale = 1.0;

static rect_t r_cancel;
static rect_t r_exit;
static int hover = 0;   /* 1 = cancel, 2 = exit */
static int pressed = 0; /* 1 = cancel, 2 = exit */

static const color_t color_bg = {17.0 / 255.0, 17.0 / 255.0, 27.0 / 255.0};
static const color_t color_border = {49.0 / 255.0, 50.0 / 255.0, 68.0 / 255.0};
static const color_t color_text = {205.0 / 255.0, 214.0 / 255.0, 244.0 / 255.0};
static const color_t color_subtext = {166.0 / 255.0, 173.0 / 255.0, 200.0 / 255.0};
static const color_t color_overlay = {108.0 / 255.0, 112.0 / 255.0, 134.0 / 255.0};
static const color_t color_cancel = {49.0 / 255.0, 50.0 / 255.0, 68.0 / 255.0};
static const color_t color_cancel_hover = {69.0 / 255.0, 71.0 / 255.0, 90.0 / 255.0};
static const color_t color_exit = {243.0 / 255.0, 139.0 / 255.0, 168.0 / 255.0};
static const color_t color_exit_hover = {245.0 / 255.0, 163.0 / 255.0, 187.0 / 255.0};

static int px(double value) {
    return (int)lrint(value * scale);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    const double pi = 3.14159265358979323846;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -pi / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, pi / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, pi / 2.0, pi);
    cairo_arc(cr, x + r, y + r, r, pi, 3.0 * pi / 2.0);
    cairo_close_path(cr);
}

static void set_source_color(cairo_t *cr, color_t color) {
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
}

static PangoLayout *make_layout(cairo_t *cr, const char *text, double size, bool bold) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, "monospace");
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    pango_font_description_set_weight(desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text, -1);
    pango_font_description_free(desc);
    return layout;
}

static void draw_text(cairo_t *cr, const char *text, double x, double y,
                      double size, bool bold, color_t color) {
    PangoLayout *layout = make_layout(cr, text, size, bold);
    set_source_color(cr, color);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static void draw_button(cairo_t *cr, rect_t rect, const char *label,
                        bool hovered, bool is_down, bool primary) {
    color_t bg = primary ? color_exit : color_cancel;
    color_t fg = primary ? color_bg : color_text;
    if (hovered) {
        bg = primary ? color_exit_hover : color_cancel_hover;
    }

    rounded_rect(cr, rect.x, rect.y, rect.w, rect.h, px(8));
    set_source_color(cr, bg);
    cairo_fill_preserve(cr);
    set_source_color(cr, primary ? color_exit : color_border);
    cairo_set_line_width(cr, px(1));
    cairo_stroke(cr);

    if (is_down) {
        /* Subtle pressed feedback: darken the button. */
        rounded_rect(cr, rect.x, rect.y, rect.w, rect.h, px(8));
        cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
        cairo_fill(cr);
    }

    PangoLayout *layout = make_layout(cr, label, px(12), primary);
    int text_w = 0, text_h = 0;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    set_source_color(cr, fg);
    cairo_move_to(cr,
                  rect.x + (rect.w - text_w) / 2.0,
                  rect.y + (rect.h - text_h) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static void redraw(void) {
    if (surface == NULL) {
        return;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    set_source_color(cr, color_bg);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const int width = px(CARD_W);
    const int height = px(CARD_H);

    rounded_rect(cr, px(0.5), px(0.5), width - px(1), height - px(1), px(CARD_RADIUS));
    set_source_color(cr, color_border);
    cairo_set_line_width(cr, px(1));
    cairo_stroke(cr);

    draw_text(cr, "Exit i3?", px(PAD_X), px(TITLE_Y), px(18), true, color_text);
    draw_text(cr, "This will terminate the current session\nand close all windows.",
              px(PAD_X), px(MESSAGE_Y), px(12), false, color_subtext);

    draw_button(cr, r_cancel, "Cancel", hover == 1, pressed == 1, false);
    draw_button(cr, r_exit, "Exit i3", hover == 2, pressed == 2, true);
    draw_text(cr, "Enter  confirm    Esc  cancel", px(PAD_X), px(HINT_Y),
              px(11), false, color_overlay);

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    xcb_flush(conn);
}

/*
 * Approximates the rounded card with one rectangle per scanline and installs
 * it as the window's bounding and input shape, so the corners are really
 * transparent even without a compositor.
 *
 */
static void apply_rounded_shape(int width, int height, int radius) {
    if (radius <= 0) {
        return;
    }

    const int max_rects = 2 * radius + 1;
    xcb_rectangle_t *rects = calloc(max_rects, sizeof(xcb_rectangle_t));
    if (rects == NULL) {
        return;
    }

    int count = 0;
    rects[count++] = (xcb_rectangle_t){0, (int16_t)radius, (uint16_t)width,
                                       (uint16_t)(height - 2 * radius)};

    for (int y = 0; y < radius; y++) {
        const double dy = radius - y - 0.5;
        const double dx = radius - sqrt((double)radius * radius - dy * dy);
        const int inset = (int)ceil(dx);
        const int w = width - 2 * inset;
        if (w <= 0) {
            continue;
        }
        rects[count++] = (xcb_rectangle_t){(int16_t)inset, (int16_t)y, (uint16_t)w, 1};
        rects[count++] = (xcb_rectangle_t){(int16_t)inset, (int16_t)(height - 1 - y),
                                           (uint16_t)w, 1};
    }

    xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, 0, win,
                         0, 0, count, rects);
    xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, 0, win,
                         0, 0, count, rects);
    free(rects);
}

static xcb_atom_t intern_atom(const char *name) {
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        conn, xcb_intern_atom(conn, 0, strlen(name), name), NULL);
    if (reply == NULL) {
        return XCB_NONE;
    }
    const xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

/*
 * Reads Xft.dpi so the card and its fonts follow the same scaling as i3.
 *
 */
static void init_scale(void) {
    const xcb_atom_t atom = intern_atom("Xft.dpi");
    if (atom == XCB_NONE) {
        return;
    }

    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        conn, xcb_get_property(conn, 0, screen->root, atom, XCB_ATOM_CARDINAL, 0, 1), NULL);
    if (reply == NULL || reply->format != 32 ||
        xcb_get_property_value_length(reply) < 4) {
        free(reply);
        return;
    }

    const double dpi = (double)*(uint32_t *)xcb_get_property_value(reply);
    free(reply);
    if (dpi >= 48.0 && dpi <= 400.0) {
        scale = dpi / 96.0;
    }
}

static void cleanup_and_exit(int status) {
    if (conn != NULL) {
        xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
        xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
        if (win != XCB_NONE) {
            xcb_destroy_window(conn, win);
        }
        if (previous_focus != XCB_NONE && previous_focus != win) {
            xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                                previous_focus, XCB_CURRENT_TIME);
        }
        xcb_flush(conn);
    }
    if (surface != NULL) {
        cairo_surface_destroy(surface);
    }
    if (conn != NULL) {
        xcb_disconnect(conn);
    }
    exit(status);
}

static void run_exit(void) {
    /* i3-msg inherits I3SOCK/DISPLAY from the i3 session. */
    if (fork() == 0) {
        setsid();
        if (fork() == 0) {
            execlp("i3-msg", "i3-msg", "exit", (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    cleanup_and_exit(0);
}

static int hit_test(int x, int y) {
    if (x >= r_cancel.x && x < r_cancel.x + r_cancel.w &&
        y >= r_cancel.y && y < r_cancel.y + r_cancel.h) {
        return 1;
    }
    if (x >= r_exit.x && x < r_exit.x + r_exit.w &&
        y >= r_exit.y && y < r_exit.y + r_exit.h) {
        return 2;
    }
    return 0;
}

int main(void) {
    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "i3-exit-dialog: cannot connect to X11\n");
        return 1;
    }

    screen = xcb_aux_get_screen(conn, 0);
    if (screen == NULL) {
        fprintf(stderr, "i3-exit-dialog: cannot get X11 screen\n");
        return 1;
    }

    init_scale();

    const int width = px(CARD_W);
    const int height = px(CARD_H);
    const int x = screen->width_in_pixels / 2 - width / 2;
    const int y = screen->height_in_pixels / 2 - height / 2;

    r_exit = (rect_t){
        .x = px(CARD_W - PAD_X - BTN_EXIT_W),
        .y = px(BUTTON_Y),
        .w = px(BTN_EXIT_W),
        .h = px(BTN_HEIGHT),
    };
    r_cancel = (rect_t){
        .x = r_exit.x - px(BTN_GAP) - px(BTN_CANCEL_W),
        .y = r_exit.y,
        .w = px(BTN_CANCEL_W),
        .h = px(BTN_HEIGHT),
    };

    const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    const uint32_t values[] = {
        screen->black_pixel,
        1,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION,
    };

    win = xcb_generate_id(conn);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                      x, y, width, height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, mask, values);

    xcb_visualtype_t *visual = xcb_aux_find_visual_by_id(screen, screen->root_visual);
    surface = cairo_xcb_surface_create(conn, win, visual, width, height);
    if (surface == NULL) {
        fprintf(stderr, "i3-exit-dialog: cannot create cairo surface\n");
        return 1;
    }

    const xcb_query_extension_reply_t *shape_ext = xcb_get_extension_data(conn, &xcb_shape_id);
    const bool have_shape = (shape_ext != NULL && shape_ext->present);
    if (have_shape) {
        apply_rounded_shape(width, height, px(CARD_RADIUS));
    }

    xcb_get_input_focus_reply_t *focus = xcb_get_input_focus_reply(
        conn, xcb_get_input_focus(conn), NULL);
    if (focus != NULL) {
        previous_focus = focus->focus;
        free(focus);
    }

    xcb_map_window(conn, win);
    xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, win, XCB_CURRENT_TIME);
    xcb_grab_keyboard(conn, 0, win, XCB_CURRENT_TIME,
                      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    xcb_grab_pointer(conn, 0, win, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                                          XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, win, XCB_NONE,
                     XCB_CURRENT_TIME);
    xcb_flush(conn);

    xcb_key_symbols_t *keysyms = xcb_key_symbols_alloc(conn);

    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(conn)) != NULL) {
        const uint8_t type = event->response_type & ~0x80;
        switch (type) {
            case XCB_EXPOSE:
                redraw();
                break;
            case XCB_KEY_PRESS: {
                xcb_key_press_event_t *key = (xcb_key_press_event_t *)event;
                xcb_keysym_t sym = xcb_key_symbols_get_keysym(keysyms, key->detail, 0);
                if (sym == KS_ESCAPE || sym == KS_Q || sym == KS_Q_UPPER) {
                    free(event);
                    cleanup_and_exit(0);
                } else if (sym == KS_RETURN || sym == KS_KP_ENTER) {
                    free(event);
                    run_exit();
                }
                break;
            }
            case XCB_MOTION_NOTIFY: {
                xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *)event;
                const int hit = hit_test(motion->event_x, motion->event_y);
                if (hit != hover) {
                    hover = hit;
                    redraw();
                }
                break;
            }
            case XCB_BUTTON_PRESS: {
                xcb_button_press_event_t *button = (xcb_button_press_event_t *)event;
                const int hit = hit_test(button->event_x, button->event_y);
                if (hit != pressed) {
                    pressed = hit;
                    redraw();
                }
                break;
            }
            case XCB_BUTTON_RELEASE: {
                xcb_button_release_event_t *button = (xcb_button_release_event_t *)event;
                const int hit = hit_test(button->event_x, button->event_y);
                const int was_pressed = pressed;
                pressed = 0;
                if (hit != 0 && hit == was_pressed) {
                    free(event);
                    if (hit == 1) {
                        cleanup_and_exit(0);
                    }
                    run_exit();
                }
                redraw();
                break;
            }
            default:
                break;
        }
        free(event);
    }

    xcb_key_symbols_free(keysyms);
    cleanup_and_exit(0);
    return 0;
}
