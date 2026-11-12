/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 * workspaces.c: Maintaining the workspace lists
 *
 */
#include "common.h"

#include <stdlib.h>
#include <string.h>

#include <yajl/yajl_parse.h>

/* A datatype to pass through the callbacks to save the state */
enum ws_key {
    WS_KEY_NONE = 0,
    WS_KEY_ID,
    WS_KEY_NUM,
    WS_KEY_NAME,
    WS_KEY_OUTPUT,
    WS_KEY_VISIBLE,
    WS_KEY_FOCUSED,
    WS_KEY_URGENT,
    WS_KEY_RECT,
    WS_KEY_X,
    WS_KEY_Y,
    WS_KEY_WIDTH,
    WS_KEY_HEIGHT,
};

struct workspaces_json_params {
    struct ws_head *workspaces;
    i3_ws *workspaces_walk;
    enum ws_key key;
    bool have_key;
    bool need_output;
    bool parsing_rect;
};

/* Case-sensitive comparison of a JSON key without allocating a string. */
static bool ws_key_is(const unsigned char *key, size_t len, const char *name) {
    return len == strlen(name) && memcmp(key, name, len) == 0;
}

/*
 * Parse a boolean value (visible, focused, urgent)
 *
 */
static int workspaces_boolean_cb(void *params_, const int val) {
    struct workspaces_json_params *params = params_;

    switch (params->key) {
        case WS_KEY_VISIBLE:
            params->workspaces_walk->visible = val;
            break;
        case WS_KEY_FOCUSED:
            params->workspaces_walk->focused = val;
            break;
        case WS_KEY_URGENT:
            params->workspaces_walk->urgent = val;
            break;
        default:
            return 0;
    }

    params->have_key = false;
    return 1;
}

/*
 * Parse an integer (num or the rect)
 *
 */
static int workspaces_integer_cb(void *params_, const long long val) {
    struct workspaces_json_params *params = params_;

    switch (params->key) {
        case WS_KEY_ID:
            params->workspaces_walk->id = val;
            break;
        case WS_KEY_NUM:
            params->workspaces_walk->num = (int)val;
            break;
        /* rect is unused, so we don't bother to save it */
        case WS_KEY_X:
        case WS_KEY_Y:
        case WS_KEY_WIDTH:
        case WS_KEY_HEIGHT:
            break;
        default:
            return 0;
    }

    params->have_key = false;
    return 1;
}

/*
 * Parse a string (name, output)
 *
 */
static int workspaces_string_cb(void *params_, const unsigned char *val, const size_t len) {
    struct workspaces_json_params *params = params_;

    if (params->key == WS_KEY_NAME) {
        const char *ws_name = (const char *)val;
        params->workspaces_walk->canonical_name = sstrndup(ws_name, len);

        if ((config.strip_ws_numbers || config.strip_ws_name) && params->workspaces_walk->num >= 0) {
            /* Special case: strip off the workspace number/name */
            static char ws_num[32];

            snprintf(ws_num, sizeof(ws_num), "%d", params->workspaces_walk->num);

            /* Calculate the length of the number str in the name */
            size_t offset = strspn(ws_name, ws_num);

            /* Also strip off the conventional ws name delimiter */
            if (offset && ws_name[offset] == ':') {
                offset += 1;
            }

            if (config.strip_ws_numbers) {
                /* Offset may be equal to length, in which case display the number */
                params->workspaces_walk->name = offset < len
                                                    ? i3string_from_markup_with_length(ws_name + offset, len - offset)
                                                    : i3string_from_markup(ws_num);
            } else {
                params->workspaces_walk->name = i3string_from_markup(ws_num);
            }
        } else {
            /* Default case: just save the name */
            params->workspaces_walk->name = i3string_from_markup_with_length(ws_name, len);
        }

        /* Save its rendered width */
        params->workspaces_walk->name_width =
            predict_text_width(params->workspaces_walk->name);

        DLOG("Got workspace canonical: %s, name: '%s', name_width: %d, glyphs: %zu\n",
             params->workspaces_walk->canonical_name,
             i3string_as_utf8(params->workspaces_walk->name),
             params->workspaces_walk->name_width,
             i3string_get_num_glyphs(params->workspaces_walk->name));

        params->have_key = false;
        return 1;
    }

    if (params->key == WS_KEY_OUTPUT) {
        /* We add the ws to the TAILQ of the output, it belongs to */
        char *output_name = NULL;
        sasprintf(&output_name, "%.*s", (int)len, val);

        i3_output *target = get_output_by_name(output_name);
        i3_ws *ws = params->workspaces_walk;
        if (target != NULL) {
            ws->output = target;
            TAILQ_INSERT_TAIL(ws->output->workspaces, ws, tailq);
        }

        params->need_output = false;
        FREE(output_name);

        params->have_key = false;
        return 1;
    }

    return 0;
}

/*
 * We hit the start of a JSON map (rect or a new workspace)
 *
 */
static int workspaces_start_map_cb(void *params_) {
    struct workspaces_json_params *params = params_;

    if (params->have_key) {
        params->parsing_rect = true;
        params->have_key = false;
    } else {
        i3_ws *new_workspace = scalloc(1, sizeof(i3_ws));
        new_workspace->num = -1;

        params->workspaces_walk = new_workspace;
        params->need_output = true;
        params->parsing_rect = false;
    }

    return 1;
}

static int workspaces_end_map_cb(void *params_) {
    struct workspaces_json_params *params = params_;

    if (params->parsing_rect) {
        params->parsing_rect = false;
        return 1;
    }

    i3_ws *ws = params->workspaces_walk;
    if (!ws || ws->output) {
        return 1; /* workspace already assigned to output */
    }

    /* If we processed the output field but didn't find the output, the
     * workspace belongs to an output this bar instance doesn't manage. */
    if (!params->need_output) {
        I3STRING_FREE(ws->name);
        FREE(ws->canonical_name);
        FREE(params->workspaces_walk);
        return 1;
    }

    if (!ws->name || SLIST_EMPTY(outputs)) { /* Invalid state */
        I3STRING_FREE(ws->name);
        FREE(ws->canonical_name);
        FREE(params->workspaces_walk);
        return 1;
    }

    /* Handle no output case - fallback to primary */
    ws->output = get_output_by_name("primary");
    if (ws->output == NULL) {
        ws->output = SLIST_FIRST(outputs);
    }
    TAILQ_INSERT_TAIL(ws->output->workspaces, ws, tailq);

    return 1;
}

/*
 * Parse a key.
 *
 * Essentially we just save it in the parsing state
 *
 */
static int workspaces_map_key_cb(void *params_, const unsigned char *keyVal, const size_t keyLen) {
    struct workspaces_json_params *params = params_;

    if (ws_key_is(keyVal, keyLen, "id")) {
        params->key = WS_KEY_ID;
    } else if (ws_key_is(keyVal, keyLen, "num")) {
        params->key = WS_KEY_NUM;
    } else if (ws_key_is(keyVal, keyLen, "name")) {
        params->key = WS_KEY_NAME;
    } else if (ws_key_is(keyVal, keyLen, "output")) {
        params->key = WS_KEY_OUTPUT;
    } else if (ws_key_is(keyVal, keyLen, "visible")) {
        params->key = WS_KEY_VISIBLE;
    } else if (ws_key_is(keyVal, keyLen, "focused")) {
        params->key = WS_KEY_FOCUSED;
    } else if (ws_key_is(keyVal, keyLen, "urgent")) {
        params->key = WS_KEY_URGENT;
    } else if (ws_key_is(keyVal, keyLen, "rect")) {
        params->key = WS_KEY_RECT;
    } else if (ws_key_is(keyVal, keyLen, "x")) {
        params->key = WS_KEY_X;
    } else if (ws_key_is(keyVal, keyLen, "y")) {
        params->key = WS_KEY_Y;
    } else if (ws_key_is(keyVal, keyLen, "width")) {
        params->key = WS_KEY_WIDTH;
    } else if (ws_key_is(keyVal, keyLen, "height")) {
        params->key = WS_KEY_HEIGHT;
    } else {
        params->key = WS_KEY_NONE;
    }

    params->have_key = true;
    return 1;
}

/* A data structure to pass all these callbacks to yajl */
static yajl_callbacks workspaces_callbacks = {
    .yajl_boolean = workspaces_boolean_cb,
    .yajl_integer = workspaces_integer_cb,
    .yajl_string = workspaces_string_cb,
    .yajl_start_map = workspaces_start_map_cb,
    .yajl_end_map = workspaces_end_map_cb,
    .yajl_map_key = workspaces_map_key_cb,
};

/*
 * Parse the received JSON string
 *
 */
void parse_workspaces_json(const unsigned char *json, const size_t size) {
    free_workspaces();

    struct workspaces_json_params params = {0};
    const yajl_handle handle = yajl_alloc(&workspaces_callbacks, NULL, &params);
    const yajl_status state = yajl_parse(handle, json, size);

    /* FIXME: Proper error handling for JSON parsing */
    switch (state) {
        case yajl_status_ok:
            break;
        case yajl_status_client_canceled:
        case yajl_status_error: {
            unsigned char *err = yajl_get_error(handle, 1, json, size);
            ELOG("Could not parse workspaces reply, error:\n%s\njson:---%s---\n", (char *)err, (char *)json);
            yajl_free_error(handle, err);

            if (config.workspace_command) {
                kill_ws_child();
                set_workspace_button_error("Could not parse workspace_command's JSON");
            } else {
                exit(EXIT_FAILURE);
            }
            break;
        }
    }

    yajl_free(handle);
}

/*
 * free() all workspace data structures. Does not free() the heads of the tailqueues.
 *
 */
void free_workspaces(void) {
    if (outputs == NULL) {
        return;
    }

    i3_output *outputs_walk;
    SLIST_FOREACH (outputs_walk, outputs, slist) {
        if (outputs_walk->workspaces != NULL && !TAILQ_EMPTY(outputs_walk->workspaces)) {
            i3_ws *ws_walk;
            TAILQ_FOREACH (ws_walk, outputs_walk->workspaces, tailq) {
                I3STRING_FREE(ws_walk->name);
                FREE(ws_walk->canonical_name);
            }
            FREE_TAILQ(outputs_walk->workspaces, i3_ws);
        }
    }
}
