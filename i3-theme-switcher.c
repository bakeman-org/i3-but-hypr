/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * i3-theme-switcher.c: native theme picker for `i3ext.themes`.
 *
 * Plain xcb + cairo + pango (no Qt, no GTK, no Python): an override-redirect,
 * keyboard-grabbing card centered on the screen listing every theme from the
 * running configuration. Up/Down (or j/k) select, Enter applies, Esc/q cancels;
 * clicking a row applies it directly. Applying runs `i3-msg theme <name>`.
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

#define PAD_X 24
#define PAD_Y 18
#define TITLE_H 30
#define ROW_H 32
#define HINT_H 28
#define CARD_RADIUS 14
#define MAX_VISIBLE_ROWS 12
#define MIN_CARD_W 320

/* Keysyms we need (avoids depending on the X11 keysym headers). */
#define KS_ESCAPE 0xff1b
#define KS_RETURN 0xff0d
#define KS_KP_ENTER 0xff8d
#define KS_UP 0xff52
#define KS_DOWN 0xff54
#define KS_HOME 0xff50
#define KS_END 0xff57
#define KS_PAGE_UP 0xff55
#define KS_PAGE_DOWN 0xff56

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
static xcb_key_symbols_t *keysyms;

static double scale = 1.0;

static char **themes;
static int theme_count;
static int current_theme = -1;
static int selected;
static int scroll_offset;
static int hover = -1;
static int pressed = -1;

static int card_w;
static int card_h;
static int rows_visible;

static const color_t color_bg = {17.0 / 255.0, 17.0 / 255.0, 27.0 / 255.0};
static const color_t color_border = {49.0 / 255.0, 50.0 / 255.0, 68.0 / 255.0};
static const color_t color_text = {205.0 / 255.0, 214.0 / 255.0, 244.0 / 255.0};
static const color_t color_subtext = {166.0 / 255.0, 173.0 / 255.0, 200.0 / 255.0};
static const color_t color_overlay = {108.0 / 255.0, 112.0 / 255.0, 134.0 / 255.0};
static const color_t color_select = {49.0 / 255.0, 50.0 / 255.0, 79.0 / 255.0};
static const color_t color_accent = {249.0 / 255.0, 226.0 / 255.0, 175.0 / 255.0};

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

static int measure_text(cairo_t *cr, const char *text, double size, bool bold) {
    PangoLayout *layout = make_layout(cr, text, size, bold);
    int width = 0;
    pango_layout_get_pixel_size(layout, &width, NULL);
    g_object_unref(layout);
    return width;
}

static void draw_text(cairo_t *cr, const char *text, double x, double y,
                      double size, bool bold, color_t color) {
    PangoLayout *layout = make_layout(cr, text, size, bold);
    set_source_color(cr, color);
    cairo_move_to(cr, x, y);
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

    rounded_rect(cr, px(0.5), px(0.5), card_w - px(1), card_h - px(1), px(CARD_RADIUS));
    set_source_color(cr, color_border);
    cairo_set_line_width(cr, px(1));
    cairo_stroke(cr);

    draw_text(cr, "Theme", px(PAD_X), px(PAD_Y), px(15), true, color_text);
    if (theme_count > 0) {
        char counter[64];
        snprintf(counter, sizeof(counter), "%d/%d", selected + 1, theme_count);
        const int width = measure_text(cr, counter, px(11), false);
        draw_text(cr, counter, card_w - px(PAD_X) - width, px(PAD_Y + 4),
                  px(11), false, color_overlay);
    }

    if (theme_count == 0) {
        draw_text(cr, "No i3ext.themes defined in the config", px(PAD_X),
                  px(PAD_Y + TITLE_H + 8), px(12), false, color_subtext);
    }

    for (int i = 0; i < rows_visible; i++) {
        const int index = scroll_offset + i;
        if (index >= theme_count) {
            break;
        }
        const int y = px(PAD_Y + TITLE_H + i * ROW_H);
        const int height = px(ROW_H - 4);

        if (index == selected) {
            rounded_rect(cr, px(PAD_X - 8), y, card_w - px(2 * (PAD_X - 8)), height, px(6));
            set_source_color(cr, color_select);
            cairo_fill(cr);
        } else if (index == hover) {
            rounded_rect(cr, px(PAD_X - 8), y, card_w - px(2 * (PAD_X - 8)), height, px(6));
            set_source_color(cr, color_border);
            cairo_fill(cr);
        }

        /* Marker for the currently applied theme. */
        const bool is_current = (index == current_theme);
        draw_text(cr, is_current ? "●" : " ",
                  px(PAD_X - 4), y + px(7), px(10), false, color_accent);
        draw_text(cr, themes[index], px(PAD_X + 14), y + px(7), px(13),
                  index == selected, index == selected ? color_text : color_subtext);
    }

    /* Scroll indicator. */
    if (theme_count > rows_visible) {
        const int track_h = px(rows_visible * ROW_H - 8);
        const int track_y = px(PAD_Y + TITLE_H + 4);
        const int thumb_h = track_h * rows_visible / theme_count;
        const int max_offset = theme_count - rows_visible;
        const int thumb_y = track_y +
                            (track_h - thumb_h) * scroll_offset / (max_offset > 0 ? max_offset : 1);
        cairo_rectangle(cr, card_w - px(8), track_y, px(3), track_h);
        set_source_color(cr, color_border);
        cairo_fill(cr);
        cairo_rectangle(cr, card_w - px(8), thumb_y, px(3), thumb_h);
        set_source_color(cr, color_overlay);
        cairo_fill(cr);
    }

    draw_text(cr, "Up/Down select   Enter apply   Esc cancel",
              px(PAD_X), card_h - px(HINT_H - 6), px(11), false, color_overlay);

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    xcb_flush(conn);
}

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

static void run_theme(int index) {
    if (index < 0 || index >= theme_count) {
        return;
    }
    /* i3-msg inherits I3SOCK/DISPLAY from the i3 session. */
    if (fork() == 0) {
        setsid();
        if (fork() == 0) {
            execlp("i3-msg", "i3-msg", "theme", themes[index], (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    cleanup_and_exit(0);
}

/* ------------------------------------------------------------------------- */
/* Theme list: read from the running i3 via `i3-msg -t get_config`. */

static char *read_command(const char *command) {
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
        return NULL;
    }

    size_t capacity = 8192;
    size_t length = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        pclose(pipe);
        return NULL;
    }

    size_t n;
    while ((n = fread(buffer + length, 1, capacity - length - 1, pipe)) > 0) {
        length += n;
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *grown = realloc(buffer, capacity);
            if (grown == NULL) {
                free(buffer);
                pclose(pipe);
                return NULL;
            }
            buffer = grown;
        }
    }
    buffer[length] = '\0';
    pclose(pipe);
    return buffer;
}

/* Parses a JSON string starting at *cursor (which must point at the opening
 * quote) into a newly allocated buffer. */
static char *parse_json_string(const char **cursor) {
    const char *p = *cursor;
    if (*p != '"') {
        return NULL;
    }
    p++;

    size_t capacity = 64;
    size_t length = 0;
    char *out = malloc(capacity);
    if (out == NULL) {
        return NULL;
    }

    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            switch (*p) {
                case '"':
                case '\\':
                case '/':
                    c = *p++;
                    break;
                case 'n':
                    c = '\n';
                    p++;
                    break;
                case 't':
                    c = '\t';
                    p++;
                    break;
                case 'u':
                    /* Non-ASCII escapes are not needed for theme names; keep a
                     * placeholder so that parsing stays in sync. */
                    c = '?';
                    p++;
                    for (int i = 0; i < 4 && *p != '\0'; i++) {
                        p++;
                    }
                    break;
                default:
                    c = *p ? *p++ : c;
                    break;
            }
        }
        if (length + 1 >= capacity) {
            capacity *= 2;
            char *grown = realloc(out, capacity);
            if (grown == NULL) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        out[length++] = c;
    }
    out[length] = '\0';
    /* Advance past the closing quote (when the string was terminated properly)
     * so that callers can continue with the next JSON token. */
    *cursor = (*p == '"') ? p + 1 : p;
    return out;
}

/* Finds `"key"` followed by ':' and returns a pointer to the value. */
static const char *find_value(const char *json, const char *key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

static void load_themes(void) {
    char *json = read_command("i3-msg -r -t get_config 2>/dev/null");
    if (json == NULL) {
        return;
    }

    const char *value = find_value(json, "themes");
    if (value != NULL && *value == '[') {
        const char *p = value + 1;
        while (*p != '\0' && *p != ']') {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') {
                p++;
            }
            if (*p != '"') {
                break;
            }
            char *name = parse_json_string(&p);
            if (name == NULL) {
                break;
            }
            char **grown = realloc(themes, sizeof(char *) * (theme_count + 1));
            if (grown == NULL) {
                free(name);
                break;
            }
            themes = grown;
            themes[theme_count++] = name;
            if (theme_count >= 128) {
                break;
            }
        }
    }

    value = find_value(json, "current_theme");
    if (value != NULL && *value == '"') {
        char *current = parse_json_string(&value);
        if (current != NULL) {
            for (int i = 0; i < theme_count; i++) {
                if (strcmp(themes[i], current) == 0) {
                    current_theme = i;
                    break;
                }
            }
            free(current);
        }
    }

    free(json);
}

/* ------------------------------------------------------------------------- */

static int hit_test(int y) {
    const int first = px(PAD_Y + TITLE_H);
    if (y < first) {
        return -1;
    }
    const int row = (y - first) / px(ROW_H);
    if (row < 0 || row >= rows_visible) {
        return -1;
    }
    const int index = scroll_offset + row;
    if (index < 0 || index >= theme_count) {
        return -1;
    }
    return index;
}

static void ensure_visible(void) {
    if (selected < scroll_offset) {
        scroll_offset = selected;
    } else if (selected >= scroll_offset + rows_visible) {
        scroll_offset = selected - rows_visible + 1;
    }
}

static void move_selection(int delta) {
    if (theme_count == 0) {
        return;
    }
    selected += delta;
    if (selected < 0) {
        selected = 0;
    }
    if (selected >= theme_count) {
        selected = theme_count - 1;
    }
    ensure_visible();
    redraw();
}

int main(void) {
    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "i3-theme-switcher: cannot connect to X11\n");
        return 1;
    }

    screen = xcb_aux_get_screen(conn, 0);
    if (screen == NULL) {
        fprintf(stderr, "i3-theme-switcher: cannot get X11 screen\n");
        return 1;
    }

    init_scale();
    load_themes();

    if (theme_count == 0) {
        fprintf(stderr, "i3-theme-switcher: no themes defined in i3ext.themes\n");
    }

    selected = (current_theme >= 0 ? current_theme : 0);
    rows_visible = theme_count < MAX_VISIBLE_ROWS ? theme_count : MAX_VISIBLE_ROWS;
    if (rows_visible > 0) {
        ensure_visible();
    }

    /* Measure the widest row with a temporary cairo context. */
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *probe_cr = cairo_create(probe);
    int widest = 0;
    for (int i = 0; i < theme_count; i++) {
        const int width = measure_text(probe_cr, themes[i], px(13), false);
        if (width > widest) {
            widest = width;
        }
    }
    cairo_destroy(probe_cr);
    cairo_surface_destroy(probe);

    card_w = widest + px(2 * PAD_X + 48);
    if (card_w < px(MIN_CARD_W)) {
        card_w = px(MIN_CARD_W);
    }
    if (card_w > screen->width_in_pixels - px(40)) {
        card_w = screen->width_in_pixels - px(40);
    }
    card_h = px(PAD_Y + TITLE_H + (rows_visible > 0 ? rows_visible : 1) * ROW_H + HINT_H);

    const int x = screen->width_in_pixels / 2 - card_w / 2;
    const int y = screen->height_in_pixels / 2 - card_h / 2;

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
                      x, y, card_w, card_h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, mask, values);

    xcb_visualtype_t *visual = xcb_aux_find_visual_by_id(screen, screen->root_visual);
    surface = cairo_xcb_surface_create(conn, win, visual, card_w, card_h);
    if (surface == NULL) {
        fprintf(stderr, "i3-theme-switcher: cannot create cairo surface\n");
        return 1;
    }

    const xcb_query_extension_reply_t *shape_ext = xcb_get_extension_data(conn, &xcb_shape_id);
    if (shape_ext != NULL && shape_ext->present) {
        apply_rounded_shape(card_w, card_h, px(CARD_RADIUS));
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

    keysyms = xcb_key_symbols_alloc(conn);

    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(conn)) != NULL) {
        const uint8_t type = event->response_type & ~0x80;
        switch (type) {
            case XCB_EXPOSE:
                redraw();
                break;
            case XCB_KEY_PRESS: {
                xcb_key_press_event_t *key = (xcb_key_press_event_t *)event;
                const xcb_keysym_t sym = xcb_key_symbols_get_keysym(keysyms, key->detail, 0);
                const bool shift = (key->state & XCB_MOD_MASK_SHIFT) != 0;

                if (sym == KS_ESCAPE || sym == 'q' || sym == 'Q') {
                    free(event);
                    cleanup_and_exit(0);
                } else if (sym == KS_RETURN || sym == KS_KP_ENTER) {
                    free(event);
                    run_theme(selected);
                } else if (sym == KS_UP || sym == 'k' || sym == 'K' || (sym == KS_PAGE_UP)) {
                    move_selection(sym == KS_PAGE_UP ? -5 : -1);
                } else if (sym == KS_DOWN || sym == 'j' || sym == 'J' || (sym == KS_PAGE_DOWN)) {
                    move_selection(sym == KS_PAGE_DOWN ? 5 : 1);
                } else if (sym == KS_HOME) {
                    selected = 0;
                    ensure_visible();
                    redraw();
                } else if (sym == KS_END) {
                    selected = theme_count - 1;
                    ensure_visible();
                    redraw();
                } else if (!shift && sym >= '1' && sym <= '9') {
                    const int index = (int)(sym - '1');
                    if (index < theme_count) {
                        free(event);
                        run_theme(index);
                    }
                }
                break;
            }
            case XCB_MOTION_NOTIFY: {
                xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *)event;
                const int hit = hit_test(motion->event_y);
                if (hit != hover) {
                    hover = hit;
                    redraw();
                }
                break;
            }
            case XCB_BUTTON_PRESS: {
                xcb_button_press_event_t *button = (xcb_button_press_event_t *)event;
                const int hit = hit_test(button->event_y);
                if (button->detail == 4) { /* scroll up */
                    move_selection(-1);
                } else if (button->detail == 5) { /* scroll down */
                    move_selection(1);
                } else if (button->detail == 1) {
                    pressed = hit;
                    if (hit >= 0) {
                        selected = hit;
                        redraw();
                    }
                }
                break;
            }
            case XCB_BUTTON_RELEASE: {
                xcb_button_release_event_t *button = (xcb_button_release_event_t *)event;
                const int hit = hit_test(button->event_y);
                const int was_pressed = pressed;
                pressed = -1;
                if (button->detail == 1 && hit >= 0 && hit == was_pressed) {
                    free(event);
                    run_theme(hit);
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
