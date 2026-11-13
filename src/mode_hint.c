/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * mode_hint.c: an on-screen hint for binding modes (e.g. the resize mode).
 *
 * The overlay is an override-redirect window at the bottom of the focused
 * output. It lists the name of the active mode and all its keyboard bindings
 * so that the user can see what the keys do without memorizing the config.
 * It never takes keyboard or pointer focus.
 *
 */
#include "all.h"

#include <cairo/cairo.h>

/* Padding and row spacing, scaled for HiDPI outputs. */
#define MH_PAD logical_px(14)
#define MH_ROW_GAP logical_px(2)
#define MH_COL_GAP logical_px(24)
#define MH_MAX_ROWS 16

struct hint_row {
    char *key;
    char *command;
};

static xcb_window_t hint_window = XCB_NONE;
static surface_t hint_surface;
static bool hint_visible = false;

static color_t hint_background(void) {
    return config.client.background;
}

static color_t hint_title_background(void) {
    return config.client.focused.background;
}

static color_t hint_title_text(void) {
    return config.client.focused.text;
}

static color_t hint_key_text(void) {
    return config.client.focused.indicator;
}

static color_t hint_command_text(void) {
    return config.client.focused.text;
}

/*
 * Formats the modifier bits of a binding as "Mod4+Shift+" etc.
 *
 */
static void format_modifiers(i3_event_state_mask_t mask, char *buf, size_t len) {
    static const struct {
        uint32_t bit;
        const char *name;
    } mods[] = {
        {XCB_MOD_MASK_SHIFT, "Shift"},
        {XCB_MOD_MASK_CONTROL, "Ctrl"},
        {XCB_MOD_MASK_1, "Mod1"},
        {XCB_MOD_MASK_2, "Mod2"},
        {XCB_MOD_MASK_3, "Mod3"},
        {XCB_MOD_MASK_4, "Mod4"},
        {XCB_MOD_MASK_5, "Mod5"},
    };

    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        if ((mask & mods[i].bit) == 0) {
            continue;
        }
        strncat(buf, mods[i].name, len - strlen(buf) - 1);
        strncat(buf, "+", len - strlen(buf) - 1);
    }
}

static void append_text(char *buf, size_t len, const char *text) {
    if (text == NULL) {
        return;
    }
    strncat(buf, text, len - strlen(buf) - 1);
}

static void free_rows(struct hint_row *rows, int count) {
    for (int i = 0; i < count; i++) {
        FREE(rows[i].key);
        FREE(rows[i].command);
    }
    free(rows);
}

void mode_hint_hide(void) {
    if (!hint_visible || hint_window == XCB_NONE) {
        return;
    }

    draw_util_surface_free(conn, &hint_surface);
    xcb_destroy_window(conn, hint_window);
    hint_window = XCB_NONE;
    hint_visible = false;
    xcb_flush(conn);
}

void mode_hint_show(const struct Mode *mode) {
    if (!config.mode_hint || mode == NULL || mode->bindings == NULL) {
        return;
    }

    int total = 0;
    Binding *bind;
    TAILQ_FOREACH (bind, mode->bindings, bindings) {
        if (bind->input_type == B_KEYBOARD) {
            total++;
        }
    }

    if (total == 0) {
        mode_hint_hide();
        return;
    }

    mode_hint_hide();

    const int shown = MIN(total, MH_MAX_ROWS);
    struct hint_row *rows = scalloc(shown, sizeof(struct hint_row));
    int index = 0;
    TAILQ_FOREACH (bind, mode->bindings, bindings) {
        if (bind->input_type != B_KEYBOARD) {
            continue;
        }
        if (index >= shown) {
            break;
        }

        char keybuf[128];
        format_modifiers(bind->event_state_mask, keybuf, sizeof(keybuf));
        if (bind->symbol != NULL) {
            append_text(keybuf, sizeof(keybuf), bind->symbol);
        } else {
            char keycode[32];
            snprintf(keycode, sizeof(keycode), "keycode %d", bind->keycode);
            append_text(keybuf, sizeof(keybuf), keycode);
        }

        rows[index].key = sstrdup(keybuf);
        const char *command = bind->command;
        while (command != NULL && (*command == ' ' || *command == '\t')) {
            command++;
        }
        rows[index].command = sstrdup(command != NULL ? command : "");
        index++;
    }

    const int line_h = config.font.height + logical_px(6);
    const int title_h = line_h + logical_px(8);

    int key_w = 0;
    int command_w = 0;
    for (int i = 0; i < shown; i++) {
        i3String *text = i3string_from_utf8(rows[i].key);
        key_w = MAX(key_w, predict_text_width(text));
        i3string_free(text);

        text = i3string_from_utf8(rows[i].command);
        command_w = MAX(command_w, predict_text_width(text));
        i3string_free(text);
    }

    char *title_text;
    sasprintf(&title_text, "mode: %s", mode->name);
    i3String *title = i3string_from_utf8(title_text);
    const int title_w = predict_text_width(title);

    /* Use the output of the focused window, so that the hint shows up where the
     * user is looking. Fall back to the whole root window. */
    Rect out = {0, 0, root_screen->width_in_pixels, root_screen->height_in_pixels};
    Output *output = (focused != NULL ? get_output_for_con(focused) : NULL);
    if (output != NULL) {
        out = output->rect;
    }

    const int margin = logical_px(24);
    int panel_w = MAX(title_w, key_w + MH_COL_GAP + command_w) + 2 * MH_PAD;
    panel_w = MIN(panel_w, (int)out.width - 2 * margin);
    panel_w = MAX(panel_w, logical_px(220));

    int panel_h = title_h + MH_PAD + shown * line_h + (shown - 1) * MH_ROW_GAP + MH_PAD;
    if (total > shown) {
        panel_h += line_h + MH_ROW_GAP;
    }

    const int panel_x = out.x + ((int)out.width - panel_w) / 2;
    int panel_y = out.y + out.height - panel_h - logical_px(56);
    if (panel_y < out.y + margin) {
        panel_y = out.y + margin;
    }

    const xcb_visualid_t visual = get_visualid_by_depth(root_depth);
    const uint32_t values[] = {
        root_screen->black_pixel,
        root_screen->black_pixel,
        1,
        colormap,
    };
    const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                          XCB_CW_OVERRIDE_REDIRECT | XCB_CW_COLORMAP;
    const Rect dims = {panel_x, panel_y, panel_w, panel_h};

    hint_window = create_window(conn, dims, root_depth, visual, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                XCURSOR_CURSOR_POINTER, false, mask, (uint32_t *)values);
    draw_util_surface_init(conn, &hint_surface, hint_window, get_visualtype_by_id(visual),
                           panel_w, panel_h);

    /* Map the window before drawing: mapping a window with a background pixel
     * set makes X11 clear it to that pixel, which would wipe out everything we
     * drew beforehand (the switcher maps first for the same reason). */
    xcb_map_window(conn, hint_window);
    const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, hint_window, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);

    draw_util_rectangle(&hint_surface, hint_background(), 0, 0, panel_w, panel_h);
    draw_util_rectangle(&hint_surface, hint_title_background(), 0, 0, panel_w, title_h);
    draw_util_text(title, &hint_surface, hint_title_text(), hint_title_background(),
                   MH_PAD, (title_h - config.font.height) / 2, panel_w - 2 * MH_PAD);
    i3string_free(title);

    int y = title_h + MH_PAD;
    for (int i = 0; i < shown; i++) {
        i3String *key = i3string_from_utf8(rows[i].key);
        draw_util_text(key, &hint_surface, hint_key_text(), hint_background(),
                       MH_PAD, y, key_w + MH_COL_GAP / 2);
        i3string_free(key);

        i3String *command = i3string_from_utf8(rows[i].command);
        draw_util_text(command, &hint_surface, hint_command_text(), hint_background(),
                       MH_PAD + key_w + MH_COL_GAP, y,
                       panel_w - MH_PAD - key_w - MH_COL_GAP - MH_PAD);
        i3string_free(command);

        y += line_h + MH_ROW_GAP;
    }

    if (total > shown) {
        char *more;
        sasprintf(&more, "+ %d more binding%s", total - shown, (total - shown == 1 ? "" : "s"));
        i3String *text = i3string_from_utf8(more);
        draw_util_text(text, &hint_surface, hint_command_text(), hint_background(),
                       MH_PAD, y, panel_w - 2 * MH_PAD);
        i3string_free(text);
        FREE(more);
    }

    CAIRO_SURFACE_FLUSH(hint_surface.surface);
    xcb_flush(conn);
    hint_visible = true;

    free_rows(rows, shown);
    FREE(title_text);
}
