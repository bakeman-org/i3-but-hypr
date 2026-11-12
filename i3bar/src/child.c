/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 * child.c: Getting input for the statusline
 *
 */
#include "common.h"
#include "queue.h"
#include "yajl_utils.h"

#include <ctype.h> /* isspace */
#include <err.h>
#include <errno.h>
#include <ev.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include <yajl/yajl_gen.h>
#include <yajl/yajl_parse.h>

/* Global variables for child_*() */
i3bar_child status_child = {0};
i3bar_child ws_child = {0};

#define DLOG_CHILD(c)                                                                                                              \
    do {                                                                                                                           \
        if ((c).pid == 0) {                                                                                                        \
            DLOG("%s: child pid = 0\n", __func__);                                                                                 \
        } else if ((c).pid == status_child.pid) {                                                                                  \
            DLOG("%s: status_command: pid=%ld stopped=%d stop_signal=%d cont_signal=%d click_events=%d click_events_init=%d\n",    \
                 __func__, (long)(c).pid, (c).stopped, (c).stop_signal, (c).cont_signal, (c).click_events, (c).click_events_init); \
        } else if ((c).pid == ws_child.pid) {                                                                                      \
            DLOG("%s: workspace_command: pid=%ld stopped=%d stop_signal=%d cont_signal=%d click_events=%d click_events_init=%d\n", \
                 __func__, (long)(c).pid, (c).stopped, (c).stop_signal, (c).cont_signal, (c).click_events, (c).click_events_init); \
        } else {                                                                                                                   \
            ELOG("%s: unknown child, this should never happen "                                                                    \
                 "pid=%ld stopped=%d stop_signal=%d cont_signal=%d click_events=%d click_events_init=%d\n",                        \
                 __func__, (long)(c).pid, (c).stopped, (c).stop_signal, (c).cont_signal, (c).click_events, (c).click_events_init); \
        }                                                                                                                          \
    } while (0)
#define DLOG_CHILDREN             \
    do {                          \
        DLOG_CHILD(status_child); \
        DLOG_CHILD(ws_child);     \
    } while (0)

/* JSON parser for stdin */
yajl_handle parser;

/* JSON generator for stdout */
yajl_gen gen;

typedef enum {
    KEY_NONE = 0,
    KEY_FULL_TEXT,
    KEY_SHORT_TEXT,
    KEY_COLOR,
    KEY_BACKGROUND,
    KEY_BORDER,
    KEY_MARKUP,
    KEY_ALIGN,
    KEY_MIN_WIDTH,
    KEY_NAME,
    KEY_INSTANCE,
    KEY_URGENT,
    KEY_SEPARATOR,
    KEY_SEPARATOR_BLOCK_WIDTH,
    KEY_BORDER_TOP,
    KEY_BORDER_RIGHT,
    KEY_BORDER_BOTTOM,
    KEY_BORDER_LEFT,
} block_key_t;

typedef struct parser_ctx {
    /* True if one of the parsed blocks was urgent */
    bool has_urgent;

    /* The current map key, interned so that parsing a status update does not
     * allocate a string for every key. */
    block_key_t key;

    /* The current block. Will be filled, then copied and put into the list of
     * blocks. */
    struct status_block block;
} parser_ctx;

parser_ctx parser_context;

struct statusline_head statusline_head = TAILQ_HEAD_INITIALIZER(statusline_head);
/* Used temporarily while reading a statusline */
struct statusline_head statusline_buffer = TAILQ_HEAD_INITIALIZER(statusline_buffer);

int child_stdin;

/*
 * Remove all blocks from the given statusline.
 * If free_resources is set, the fields of each status block will be free'd.
 */
void clear_statusline(struct statusline_head *head, bool free_resources) {
    struct status_block *first;
    while (!TAILQ_EMPTY(head)) {
        first = TAILQ_FIRST(head);
        if (free_resources) {
            I3STRING_FREE(first->full_text);
            I3STRING_FREE(first->short_text);
            FREE(first->color);
            FREE(first->name);
            FREE(first->instance);
            FREE(first->min_width_str);
            FREE(first->background);
            FREE(first->border);
        }

        TAILQ_REMOVE(head, first, blocks);
        free(first);
    }
}

/*
 * Move all blocks from one statusline to another. This avoids the previous
 * allocate-and-copy of every block (and its whole owning structure) on each
 * status update.
 */
static void move_statusline(struct statusline_head *from, struct statusline_head *to) {
    struct status_block *current;
    while ((current = TAILQ_FIRST(from)) != NULL) {
        TAILQ_REMOVE(from, current, blocks);
        TAILQ_INSERT_TAIL(to, current, blocks);
    }
}

/*
 * Replaces the statusline in memory with an error message. Pass a format
 * string and format parameters as you would in `printf'. The next time
 * `draw_bars' is called, the error message text will be drawn on the bar in
 * the space allocated for the statusline.
 */
__attribute__((format(printf, 1, 2))) static void set_statusline_error(const char *format, ...) {
    clear_statusline(&statusline_head, true);

    char *message;
    va_list args;
    va_start(args, format);
    if (vasprintf(&message, format, args) == -1) {
        goto finish;
    }

    struct status_block *err_block = scalloc(1, sizeof(struct status_block));
    err_block->full_text = i3string_from_utf8("Error: ");
    err_block->name = sstrdup("error");
    err_block->color = sstrdup("#ff0000");
    err_block->no_separator = true;

    struct status_block *message_block = scalloc(1, sizeof(struct status_block));
    message_block->full_text = i3string_from_utf8(message);
    message_block->name = sstrdup("error_message");
    message_block->color = sstrdup("#ff0000");
    message_block->no_separator = true;

    TAILQ_INSERT_HEAD(&statusline_head, err_block, blocks);
    TAILQ_INSERT_TAIL(&statusline_head, message_block, blocks);

finish:
    free(message);
    va_end(args);
}

/*
 * Stop and free() the stdin- and SIGCHLD-watchers
 *
 */
static void cleanup(i3bar_child *c) {
    DLOG_CHILD(*c);

    if (c->stdin_io != NULL) {
        ev_io_stop(main_loop, c->stdin_io);
        FREE(c->stdin_io);

        if (c->pid == status_child.pid) {
            close(child_stdin);
            child_stdin = 0;
        }
        close(c->stdin_fd);
    }

    if (c->child_sig != NULL) {
        ev_child_stop(main_loop, c->child_sig);
        FREE(c->child_sig);
    }

    FREE(c->pending_line);
    memset(c, 0, sizeof(i3bar_child));
}

/*
 * The start of a new array is the start of a new status line, so we clear all
 * previous entries from the buffer.
 */
static int stdin_start_array(void *context) {
    // the blocks are still used by statusline_head, so we won't free the
    // resources here.
    clear_statusline(&statusline_buffer, false);
    return 1;
}

/*
 * The start of a map is the start of a single block of the status line.
 *
 */
static int stdin_start_map(void *context) {
    parser_ctx *ctx = context;
    memset(&(ctx->block), '\0', sizeof(struct status_block));

    /* Default width of the separator block. */
    if (config.separator_symbol == NULL) {
        ctx->block.sep_block_width = logical_px(9);
    } else {
        ctx->block.sep_block_width = logical_px(8) + separator_symbol_width;
    }

    /* By default we draw all four borders if a border is set. */
    ctx->block.border_top = 1;
    ctx->block.border_right = 1;
    ctx->block.border_bottom = 1;
    ctx->block.border_left = 1;

    return 1;
}

/* Case-insensitive comparison of a JSON key without allocating a string. */
static bool key_is(const unsigned char *key, size_t len, const char *name) {
    return len == strlen(name) && strncasecmp((const char *)key, name, len) == 0;
}

static int stdin_map_key(void *context, const unsigned char *key, size_t len) {
    parser_ctx *ctx = context;

    if (key_is(key, len, "full_text")) {
        ctx->key = KEY_FULL_TEXT;
    } else if (key_is(key, len, "short_text")) {
        ctx->key = KEY_SHORT_TEXT;
    } else if (key_is(key, len, "color")) {
        ctx->key = KEY_COLOR;
    } else if (key_is(key, len, "background")) {
        ctx->key = KEY_BACKGROUND;
    } else if (key_is(key, len, "border")) {
        ctx->key = KEY_BORDER;
    } else if (key_is(key, len, "markup")) {
        ctx->key = KEY_MARKUP;
    } else if (key_is(key, len, "align")) {
        ctx->key = KEY_ALIGN;
    } else if (key_is(key, len, "min_width")) {
        ctx->key = KEY_MIN_WIDTH;
    } else if (key_is(key, len, "name")) {
        ctx->key = KEY_NAME;
    } else if (key_is(key, len, "instance")) {
        ctx->key = KEY_INSTANCE;
    } else if (key_is(key, len, "urgent")) {
        ctx->key = KEY_URGENT;
    } else if (key_is(key, len, "separator")) {
        ctx->key = KEY_SEPARATOR;
    } else if (key_is(key, len, "separator_block_width")) {
        ctx->key = KEY_SEPARATOR_BLOCK_WIDTH;
    } else if (key_is(key, len, "border_top")) {
        ctx->key = KEY_BORDER_TOP;
    } else if (key_is(key, len, "border_right")) {
        ctx->key = KEY_BORDER_RIGHT;
    } else if (key_is(key, len, "border_bottom")) {
        ctx->key = KEY_BORDER_BOTTOM;
    } else if (key_is(key, len, "border_left")) {
        ctx->key = KEY_BORDER_LEFT;
    } else {
        ctx->key = KEY_NONE;
    }

    return 1;
}

static int stdin_boolean(void *context, int val) {
    parser_ctx *ctx = context;

    switch (ctx->key) {
        case KEY_URGENT:
            ctx->block.urgent = val;
            break;
        case KEY_SEPARATOR:
            ctx->block.no_separator = !val;
            break;
        default:
            break;
    }

    return 1;
}

static int stdin_string(void *context, const unsigned char *val, size_t len) {
    parser_ctx *ctx = context;

    switch (ctx->key) {
        case KEY_FULL_TEXT:
            ctx->block.full_text = i3string_from_markup_with_length((const char *)val, len);
            break;
        case KEY_SHORT_TEXT:
            ctx->block.short_text = i3string_from_markup_with_length((const char *)val, len);
            break;
        case KEY_COLOR:
            sasprintf(&(ctx->block.color), "%.*s", (int)len, val);
            break;
        case KEY_BACKGROUND:
            sasprintf(&(ctx->block.background), "%.*s", (int)len, val);
            break;
        case KEY_BORDER:
            sasprintf(&(ctx->block.border), "%.*s", (int)len, val);
            break;
        case KEY_MARKUP:
            ctx->block.pango_markup = (len == strlen("pango") && !strncasecmp((const char *)val, "pango", strlen("pango")));
            break;
        case KEY_ALIGN:
            if (len == strlen("center") && !strncmp((const char *)val, "center", strlen("center"))) {
                ctx->block.align = ALIGN_CENTER;
            } else if (len == strlen("right") && !strncmp((const char *)val, "right", strlen("right"))) {
                ctx->block.align = ALIGN_RIGHT;
            } else {
                ctx->block.align = ALIGN_LEFT;
            }
            break;
        case KEY_MIN_WIDTH:
            sasprintf(&(ctx->block.min_width_str), "%.*s", (int)len, val);
            break;
        case KEY_NAME:
            sasprintf(&(ctx->block.name), "%.*s", (int)len, val);
            break;
        case KEY_INSTANCE:
            sasprintf(&(ctx->block.instance), "%.*s", (int)len, val);
            break;
        default:
            break;
    }

    return 1;
}

static int stdin_integer(void *context, long long val) {
    parser_ctx *ctx = context;

    switch (ctx->key) {
        case KEY_MIN_WIDTH:
            ctx->block.min_width = (uint32_t)val;
            break;
        case KEY_SEPARATOR_BLOCK_WIDTH:
            ctx->block.sep_block_width = (uint32_t)val;
            break;
        case KEY_BORDER_TOP:
            ctx->block.border_top = (uint32_t)val;
            break;
        case KEY_BORDER_RIGHT:
            ctx->block.border_right = (uint32_t)val;
            break;
        case KEY_BORDER_BOTTOM:
            ctx->block.border_bottom = (uint32_t)val;
            break;
        case KEY_BORDER_LEFT:
            ctx->block.border_left = (uint32_t)val;
            break;
        default:
            break;
    }

    return 1;
}

/*
 * When a map is finished, we have an entire status block.
 * Move it from the parser's context to the statusline buffer.
 */
static int stdin_end_map(void *context) {
    parser_ctx *ctx = context;
    struct status_block *new_block = smalloc(sizeof(struct status_block));
    memcpy(new_block, &(ctx->block), sizeof(struct status_block));
    /* Ensure we have a full_text set, so that when it is missing (or null),
     * i3bar doesn’t crash and the user gets an annoying message. */
    if (!new_block->full_text) {
        new_block->full_text = i3string_from_utf8("SPEC VIOLATION: full_text is NULL!");
    }
    if (new_block->urgent) {
        ctx->has_urgent = true;
    }

    if (new_block->min_width_str) {
        i3String *text = i3string_from_utf8(new_block->min_width_str);
        i3string_set_markup(text, new_block->pango_markup);
        new_block->min_width = (uint32_t)predict_text_width(text);
        i3string_free(text);
    }

    i3string_set_markup(new_block->full_text, new_block->pango_markup);

    new_block->use_short = false;
    if (new_block->short_text != NULL) {
        i3string_set_markup(new_block->short_text, new_block->pango_markup);
    }

    TAILQ_INSERT_TAIL(&statusline_buffer, new_block, blocks);

    return 1;
}

/*
 * When an array is finished, we have an entire statusline.
 * Move it from the buffer to the actual statusline.
 */
static int stdin_end_array(void *context) {
    DLOG("moving statusline_buffer to statusline_head\n");
    clear_statusline(&statusline_head, true);
    move_statusline(&statusline_buffer, &statusline_head);

    DLOG("dumping statusline:\n");
    struct status_block *current;
    TAILQ_FOREACH (current, &statusline_head, blocks) {
        DLOG("full_text = %s\n", i3string_as_utf8(current->full_text));
        DLOG("short_text = %s\n", (current->short_text == NULL ? NULL : i3string_as_utf8(current->short_text)));
        DLOG("color = %s\n", current->color);
    }
    DLOG("end of dump\n");
    return 1;
}

/*
 * Helper function to read stdin
 *
 * Returns NULL on EOF. The returned buffer is owned by i3bar and reused for
 * subsequent reads; callers must not free it.
 *
 */
static unsigned char *stdin_read_buffer;
static size_t stdin_read_capacity;

static unsigned char *get_buffer(int fd, int *ret_buffer_len) {
    if (stdin_read_buffer == NULL) {
        /* Start big enough for typical status lines to avoid growing the
         * buffer on every update. */
        stdin_read_capacity = 4096;
        stdin_read_buffer = smalloc(stdin_read_capacity + 1);
    }

    int rec = 0;
    while (1) {
        const ssize_t n = read(fd, stdin_read_buffer + rec, stdin_read_capacity - rec);
        if (n == -1) {
            if (errno == EAGAIN) {
                /* finish up */
                break;
            }
            ELOG("read() failed!: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (n == 0) {
            ELOG("stdin: received EOF\n");
            *ret_buffer_len = -1;
            return NULL;
        }
        rec += n;

        if ((size_t)rec == stdin_read_capacity) {
            /* Grow exponentially instead of by a fixed chunk. */
            stdin_read_capacity *= 2;
            stdin_read_buffer = srealloc(stdin_read_buffer, stdin_read_capacity + 1);
        }
    }
    stdin_read_buffer[rec] = '\0';
    if (stdin_read_buffer[0] == '\0') {
        rec = -1;
    }
    *ret_buffer_len = rec;
    return stdin_read_buffer;
}

static void read_flat_input(char *buffer, int length) {
    struct status_block *first = TAILQ_FIRST(&statusline_head);
    /* Clear the old buffer if any. */
    I3STRING_FREE(first->full_text);
    /* Remove the trailing newline and terminate the string at the same
     * time. */
    if (buffer[length - 1] == '\n' || buffer[length - 1] == '\r') {
        buffer[length - 1] = '\0';
    } else {
        buffer[length] = '\0';
    }

    first->full_text = i3string_from_utf8(buffer);
}

static bool read_json_input(unsigned char *input, int length) {
    yajl_status status = yajl_parse(parser, input, length);
    bool has_urgent = false;
    if (status != yajl_status_ok) {
        char *message = (char *)yajl_get_error(parser, 0, input, length);

        /* strip the newline yajl adds to the error message */
        if (message[strlen(message) - 1] == '\n') {
            message[strlen(message) - 1] = '\0';
        }

        fprintf(stderr, "[i3bar] Could not parse JSON input (code = %d, message = %s): %.*s\n",
                status, message, length, input);

        set_statusline_error("Could not parse JSON (%s)", message);
        yajl_free_error(parser, (unsigned char *)message);
        draw_bars(false);
    } else if (parser_context.has_urgent) {
        has_urgent = true;
    }
    return has_urgent;
}

/*
 * Cheap fingerprint of the last status update. Many status commands only emit
 * a new line when a value actually changes; in that case the payload is
 * byte-identical to the previous one and neither the JSON parser nor a full
 * bar redraw has to run.
 */
static uint64_t last_status_hash;
static size_t last_status_length;
static bool last_status_hash_valid;

static uint64_t status_hash(const unsigned char *data, size_t length) {
    /* FNV-1a, 64 bit. */
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < length; i++) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool status_payload_unchanged(const unsigned char *buffer, int length) {
    if (length <= 0) {
        return false;
    }

    /* Only remember complete-looking updates: a fragment of a larger JSON
     * document must never be skipped because it happens to repeat. */
    const unsigned char last = buffer[length - 1];
    if (last != ']' && last != '}' && last != ',' && last != '\n' && last != '\r') {
        return false;
    }

    const uint64_t hash = status_hash(buffer, (size_t)length);
    if (last_status_hash_valid && hash == last_status_hash &&
        (size_t)length == last_status_length) {
        return true;
    }

    last_status_hash = hash;
    last_status_length = (size_t)length;
    last_status_hash_valid = true;
    return false;
}

/*
 * Callbalk for stdin. We read a line from stdin and store the result
 * in statusline
 *
 */
static void stdin_io_cb(int fd) {
    int rec;
    unsigned char *buffer = get_buffer(fd, &rec);
    if (buffer == NULL) {
        return;
    }
    if (status_payload_unchanged(buffer, rec)) {
        DLOG("status update unchanged, skipping parse and redraw\n");
        return;
    }
    bool has_urgent = false;
    if (status_child.version > 0) {
        has_urgent = read_json_input(buffer, rec);
    } else {
        read_flat_input((char *)buffer, rec);
    }
    draw_bars(has_urgent);
}

/*
 * Callbalk for stdin first line. We read the first line to detect
 * whether this is JSON or plain text
 *
 */
static void stdin_io_first_line_cb(int fd) {
    int rec;
    unsigned char *buffer = get_buffer(fd, &rec);
    if (buffer == NULL) {
        return;
    }
    DLOG("Detecting input type based on buffer *%.*s*\n", rec, buffer);
    /* Detect whether this is JSON or plain text. */
    unsigned int consumed = 0;
    /* At the moment, we don’t care for the version. This might change
     * in the future, but for now, we just discard it. */
    parse_json_header(&status_child, buffer, rec, &consumed);
    if (status_child.version > 0) {
        /* If hide-on-modifier is set, we start of by sending the status_child
         * a SIGSTOP, because the bars aren't mapped at start */
        if (config.hide_on_modifier) {
            stop_children();
        }
        draw_bars(read_json_input(buffer + consumed, rec - consumed));
    } else {
        /* In case of plaintext, we just add a single block and change its
         * full_text pointer later. */
        struct status_block *new_block = scalloc(1, sizeof(struct status_block));
        TAILQ_INSERT_TAIL(&statusline_head, new_block, blocks);
        read_flat_input((char *)buffer, rec);
    }
}

static bool isempty(char *s) {
    while (*s != '\0') {
        if (!isspace((unsigned char)*s)) {
            return false;
        }
        s++;
    }
    return true;
}

static char *append_string(const char *previous, const char *str) {
    if (previous != NULL) {
        char *result;
        sasprintf(&result, "%s%s", previous, str);
        return result;
    }
    return sstrdup(str);
}

static char *ws_last_json;

static void ws_stdin_io_cb(int fd) {
    int rec;
    unsigned char *buffer = get_buffer(fd, &rec);
    if (buffer == NULL) {
        return;
    }

    /* Split on newlines in place instead of letting GLib allocate an array
     * plus a copy of every line. */
    char *line = (char *)buffer;
    for (;;) {
        char *newline = strchr(line, '\n');
        if (newline == NULL) {
            /* This is the last (possibly empty) segment. If it is not empty,
             * we have read incomplete data and save it for later. */
            if (ws_child.pending_line == NULL && isempty(line)) {
                break;
            }
            char *pending = append_string(ws_child.pending_line, line);
            free(ws_child.pending_line);
            ws_child.pending_line = pending;
            break;
        }

        *newline = '\0';
        if (ws_child.pending_line == NULL && isempty(line)) {
            /* In the normal case where the buffer ends with '\n', the trailing
             * segment is empty. */
            line = newline + 1;
            continue;
        }

        free(ws_last_json);
        ws_last_json = append_string(ws_child.pending_line, line);
        FREE(ws_child.pending_line);

        parse_workspaces_json((const unsigned char *)ws_last_json, strlen(ws_last_json));

        line = newline + 1;
    }

    draw_bars(false);
}

static void common_stdin_cb(struct ev_loop *loop, ev_io *watcher, int revents) {
    if (watcher == status_child.stdin_io) {
        if (status_child.version == (uint32_t)-1) {
            stdin_io_first_line_cb(watcher->fd);
        } else {
            stdin_io_cb(watcher->fd);
        }
    } else if (watcher == ws_child.stdin_io) {
        ws_stdin_io_cb(watcher->fd);
    } else {
        ELOG("Got callback for unknown watcher fd=%d\n", watcher->fd);
    }
}

/*
 * When workspace_command is enabled this function is used to re-parse the
 * latest received JSON from the client.
 */
void repeat_last_ws_json(void) {
    if (ws_last_json) {
        DLOG("Repeating last workspace JSON\n");
        parse_workspaces_json((const unsigned char *)ws_last_json, strlen(ws_last_json));
    }
}

/*
 * Wrapper around set_workspace_button_error to mimic the call of
 * set_statusline_error.
 */
__attribute__((format(printf, 1, 2))) static void set_workspace_button_error_f(const char *format, ...) {
    char *message;
    va_list args;
    va_start(args, format);
    if (vasprintf(&message, format, args) == -1) {
        goto finish;
    }

    set_workspace_button_error(message);

finish:
    free(message);
    va_end(args);
}

/*
 * Replaces the workspace buttons with an error message.
 */
void set_workspace_button_error(const char *message) {
    free_workspaces();

    char *name = NULL;
    sasprintf(&name, "Error: %s", message);

    i3_output *output;
    SLIST_FOREACH (output, outputs, slist) {
        i3_ws *fake_ws = scalloc(1, sizeof(i3_ws));
        /* Don't set the canonical_name field to make this workspace unfocusable. */
        fake_ws->name = i3string_from_utf8(name);
        fake_ws->name_width = predict_text_width(fake_ws->name);
        fake_ws->num = -1;
        fake_ws->urgent = fake_ws->visible = true;
        fake_ws->output = output;

        TAILQ_INSERT_TAIL(output->workspaces, fake_ws, tailq);
    }

    free(name);
}

/*
 * We received a SIGCHLD, meaning, that the child process terminated.
 * We simply free the respective data structures and don't care for input
 * anymore
 *
 */
static void child_sig_cb(struct ev_loop *loop, ev_child *watcher, int revents) {
    const int exit_status = WEXITSTATUS(watcher->rstatus);

    ELOG("Child (pid: %d) unexpectedly exited with status %d\n",
         watcher->pid,
         exit_status);

    __attribute__((format(printf, 1, 2))) void (*error_function_pointer)(const char *, ...) = NULL;
    const char *command_type = "";
    i3bar_child *c = NULL;
    if (watcher->pid == status_child.pid) {
        command_type = "status_command";
        error_function_pointer = set_statusline_error;
        c = &status_child;
    } else if (watcher->pid == ws_child.pid) {
        command_type = "workspace_command";
        error_function_pointer = set_workspace_button_error_f;
        c = &ws_child;
    } else {
        ELOG("Unknown child pid, this should never happen\n");
        return;
    }
    DLOG_CHILD(*c);

    /* this error is most likely caused by a user giving a nonexecutable or
     * nonexistent file, so we will handle those cases separately. */
    if (exit_status == 126) {
        error_function_pointer("%s is not executable (exit %d)", command_type, exit_status);
    } else if (exit_status == 127) {
        error_function_pointer("%s not found or is missing a library dependency (exit %d)", command_type, exit_status);
    } else {
        error_function_pointer("%s process exited unexpectedly (exit %d)", command_type, exit_status);
    }

    cleanup(c);
    draw_bars(false);
}

static void child_write_output(void) {
    if (status_child.click_events) {
        const unsigned char *output;
        size_t size;
        ssize_t n;

        yajl_gen_get_buf(gen, &output, &size);

        n = writeall(child_stdin, output, size);
        if (n != -1) {
            n = writeall(child_stdin, "\n", 1);
        }

        yajl_gen_clear(gen);

        if (n == -1) {
            status_child.click_events = false;
            kill_child();
            set_statusline_error("child_write_output failed");
            draw_bars(false);
        }
    }
}

static pid_t sfork(void) {
    const pid_t pid = fork();
    if (pid == -1) {
        ELOG("Couldn't fork(): %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    return pid;
}

static void spipe(int pipedes[2]) {
    if (pipe(pipedes) == -1) {
        err(EXIT_FAILURE, "pipe(pipe_in)");
    }
}

static void exec_shell(char *command) {
    execl(_PATH_BSHELL, _PATH_BSHELL, "-c", command, (char *)NULL);
    err(EXIT_FAILURE, "execl return"); /* only reached on error */
}

static void setup_child_cb(i3bar_child *child) {
    /* We set O_NONBLOCK because blocking is evil in event-driven software */
    fcntl(child->stdin_fd, F_SETFL, O_NONBLOCK);

    child->stdin_io = smalloc(sizeof(ev_io));
    ev_io_init(child->stdin_io, &common_stdin_cb, child->stdin_fd, EV_READ);
    ev_io_start(main_loop, child->stdin_io);

    /* We must cleanup, if the child unexpectedly terminates */
    child->child_sig = smalloc(sizeof(ev_child));
    ev_child_init(child->child_sig, &child_sig_cb, child->pid, 0);
    ev_child_start(main_loop, child->child_sig);

    DLOG_CHILD(*child);
}

/*
 * Start a child process with the specified command and reroute stdin.
 * We actually start a shell to execute the command so we don't have to care
 * about arguments and such.
 *
 * If `command' is NULL, such as in the case when no `status_command' is given
 * in the bar config, no child will be started.
 *
 */
void start_child(char *command) {
    if (command == NULL) {
        return;
    }

    /* Allocate a yajl parser which will be used to parse stdin. */
    static yajl_callbacks callbacks = {
        .yajl_boolean = stdin_boolean,
        .yajl_integer = stdin_integer,
        .yajl_string = stdin_string,
        .yajl_start_map = stdin_start_map,
        .yajl_map_key = stdin_map_key,
        .yajl_end_map = stdin_end_map,
        .yajl_start_array = stdin_start_array,
        .yajl_end_array = stdin_end_array,
    };
    parser = yajl_alloc(&callbacks, NULL, &parser_context);
    /* Allow multiple values: the status line is a stream of JSON arrays, one
     * per update. yajl 1 tolerated this implicitly, yajl 2 needs the flag. */
    yajl_config(parser, yajl_allow_multiple_values, 1);
    gen = yajl_gen_alloc(NULL);

    int pipe_in[2];  /* pipe we read from */
    int pipe_out[2]; /* pipe we write to */
    spipe(pipe_in);
    spipe(pipe_out);

    status_child.pid = sfork();
    if (status_child.pid == 0) {
        /* Child-process. Reroute streams and start shell */
        close(pipe_in[0]);
        close(pipe_out[1]);

        dup2(pipe_in[1], STDOUT_FILENO);
        dup2(pipe_out[0], STDIN_FILENO);

        setpgid(status_child.pid, 0);
        exec_shell(command);
        return;
    }
    /* Parent-process. Reroute streams */
    close(pipe_in[1]);
    close(pipe_out[0]);

    status_child.stdin_fd = pipe_in[0];
    child_stdin = pipe_out[1];
    status_child.version = -1;

    setup_child_cb(&status_child);
}

/*
 * Same as start_child but starts the configured client that manages workspace
 * buttons.
 *
 */
void start_ws_child(char *command) {
    if (command == NULL) {
        return;
    }

    ws_child.stop_signal = SIGSTOP;
    ws_child.cont_signal = SIGCONT;

    int pipe_in[2]; /* pipe we read from */
    spipe(pipe_in);

    ws_child.pid = sfork();
    if (ws_child.pid == 0) {
        /* Child-process. Reroute streams and start shell */
        close(pipe_in[0]);
        dup2(pipe_in[1], STDOUT_FILENO);

        setpgid(ws_child.pid, 0);
        exec_shell(command);
    }
    /* Parent-process. Reroute streams */
    close(pipe_in[1]);
    ws_child.stdin_fd = pipe_in[0];

    setup_child_cb(&ws_child);
}

static void child_click_events_initialize(void) {
    DLOG_CHILD(status_child);

    if (!status_child.click_events_init) {
        yajl_gen_array_open(gen);
        child_write_output();
        status_child.click_events_init = true;
    }
}

/*
 * Generates a click event, if enabled.
 *
 */
void send_block_clicked(int button, const char *name, const char *instance, int x, int y, int x_rel, int y_rel, int out_x, int out_y, int width, int height, int mods) {
    if (!status_child.click_events) {
        return;
    }

    child_click_events_initialize();

    yajl_gen_map_open(gen);

    if (name) {
        ystr("name");
        ystr(name);
    }

    if (instance) {
        ystr("instance");
        ystr(instance);
    }

    ystr("button");
    yajl_gen_integer(gen, button);

    ystr("modifiers");
    yajl_gen_array_open(gen);
    if (mods & XCB_MOD_MASK_SHIFT) {
        ystr("Shift");
    }
    if (mods & XCB_MOD_MASK_CONTROL) {
        ystr("Control");
    }
    if (mods & XCB_MOD_MASK_1) {
        ystr("Mod1");
    }
    if (mods & XCB_MOD_MASK_2) {
        ystr("Mod2");
    }
    if (mods & XCB_MOD_MASK_3) {
        ystr("Mod3");
    }
    if (mods & XCB_MOD_MASK_4) {
        ystr("Mod4");
    }
    if (mods & XCB_MOD_MASK_5) {
        ystr("Mod5");
    }
    yajl_gen_array_close(gen);

    ystr("x");
    yajl_gen_integer(gen, x);

    ystr("y");
    yajl_gen_integer(gen, y);

    ystr("relative_x");
    yajl_gen_integer(gen, x_rel);

    ystr("relative_y");
    yajl_gen_integer(gen, y_rel);

    ystr("output_x");
    yajl_gen_integer(gen, out_x);

    ystr("output_y");
    yajl_gen_integer(gen, out_y);

    ystr("width");
    yajl_gen_integer(gen, width);

    ystr("height");
    yajl_gen_integer(gen, height);

    yajl_gen_map_close(gen);
    child_write_output();
}

static bool is_alive(i3bar_child *c) {
    return c->pid > 0;
}

/*
 * Returns true if the status child process is alive.
 *
 */
bool status_child_is_alive(void) {
    return is_alive(&status_child);
}

/*
 * Returns true if the workspace child process is alive.
 *
 */
bool ws_child_is_alive(void) {
    return is_alive(&ws_child);
}

/*
 * kill()s the child process (if any). Called when exit()ing.
 *
 */
void kill_children_at_exit(void) {
    DLOG_CHILDREN;
    cont_children();

    if (is_alive(&status_child)) {
        killpg(status_child.pid, SIGTERM);
    }
    if (is_alive(&ws_child)) {
        killpg(ws_child.pid, SIGTERM);
    }
}

static void cont_child(i3bar_child *c) {
    if (is_alive(c) && c->cont_signal > 0 && c->stopped) {
        c->stopped = false;
        killpg(c->pid, c->cont_signal);
    }
}

static void kill_and_wait(i3bar_child *c) {
    DLOG_CHILD(*c);
    if (!is_alive(c)) {
        return;
    }

    cont_child(c);
    killpg(c->pid, SIGTERM);
    int status;
    waitpid(c->pid, &status, 0);
    cleanup(c);
}

/*
 * kill()s the child process (if any) and closes and free()s the stdin- and
 * SIGCHLD-watchers
 *
 */
void kill_child(void) {
    kill_and_wait(&status_child);
}

/*
 * kill()s the workspace child process (if any) and closes and free()s the
 * stdin- and SIGCHLD-watchers.
 * Similar to kill_child.
 *
 */
void kill_ws_child(void) {
    kill_and_wait(&ws_child);
}

static void stop_child(i3bar_child *c) {
    if (c->stop_signal > 0 && !c->stopped) {
        c->stopped = true;
        killpg(c->pid, c->stop_signal);
    }
}

/*
 * Sends a SIGSTOP to the child process (if existent)
 *
 */
void stop_children(void) {
    DLOG_CHILDREN;
    stop_child(&status_child);
    stop_child(&ws_child);
}

/*
 * Sends a SIGCONT to the child process (if existent)
 *
 */
void cont_children(void) {
    DLOG_CHILDREN;

    cont_child(&status_child);
    cont_child(&ws_child);
}

/*
 * Whether or not the child want click events
 *
 */
bool child_want_click_events(void) {
    return status_child.click_events;
}
