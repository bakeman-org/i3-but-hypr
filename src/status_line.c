/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * status_line.c: publishes a compact WM state string for i3bar.
 *
 * The text is sent as a "mode" IPC event, which i3bar renders in its existing
 * binding-mode block. That keeps the state visible in the bar instead of
 * covering window contents with an overlay window.
 *
 */
#include "all.h"

/* Cached text, so that we only publish when something changed. */
static char line_text[1024] = "";
static char last_command[512] = "";

static void utf8_truncate_tail(char *str);

void status_line_set_last_command(const char *command) {
    if (command == NULL) {
        last_command[0] = '\0';
        return;
    }

    /* Collapse newlines/tabs so that the text stays a single line. */
    size_t out = 0;
    for (size_t i = 0; command[i] != '\0' && out + 1 < sizeof(last_command); i++) {
        const unsigned char c = (unsigned char)command[i];
        last_command[out++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (char)c;
    }
    last_command[out] = '\0';
    utf8_truncate_tail(last_command);
}

static void publish(const char *change) {
    if (conn == NULL) {
        return;
    }

    /* Escape for the JSON payload of the IPC event. */
    char escaped[2048];
    size_t out = 0;
    for (size_t i = 0; change[i] != '\0' && out + 2 < sizeof(escaped); i++) {
        const unsigned char c = (unsigned char)change[i];
        if (c == '"' || c == '\\') {
            escaped[out++] = '\\';
            escaped[out++] = (char)c;
        } else if (c < 0x20) {
            escaped[out++] = ' ';
        } else {
            escaped[out++] = (char)c;
        }
    }
    escaped[out] = '\0';

    char *msg;
    sasprintf(&msg, "{\"change\":\"%s\", \"pango_markup\":false}", escaped);
    ipc_send_event("mode", I3_IPC_EVENT_MODE, msg);
    free(msg);
}

void status_line_hide(void) {
    line_text[0] = '\0';
    /* Restore the plain binding mode in the bar. */
    publish(current_binding_mode != NULL ? current_binding_mode : DEFAULT_BINDING_MODE);
}

static const char *layout_name(layout_t layout) {
    switch (layout) {
        case L_DEFAULT:
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

    char short_last[64];
    size_t last_len = strlen(last_command);
    if (last_len > sizeof(short_last) - 1) {
        last_len = sizeof(short_last) - 1;
    }
    memcpy(short_last, last_command, last_len);
    short_last[last_len] = '\0';
    utf8_truncate_tail(short_last);

    snprintf(buf, len, "[%s] %s | %s | %s%s | last: %s",
             current_binding_mode != NULL ? current_binding_mode : "default",
             ws_name, layout, short_title, state,
             short_last[0] != '\0' ? short_last : "-");
}

void status_line_update(void) {
    if (!config.status_line || conn == NULL || root_screen == NULL) {
        return;
    }

    char text[sizeof(line_text)];
    build_text(text, sizeof(text));
    if (strcmp(text, line_text) == 0) {
        return;
    }

    snprintf(line_text, sizeof(line_text), "%s", text);
    publish(text);
}

void status_line_refresh(void) {
    line_text[0] = '\0';
    status_line_update();
}
