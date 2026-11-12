/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * dropdown.c: Yakuake/niri-style dropdown windows. Windows are hidden by
 *             moving them to the scratchpad and shown again at a fixed panel
 *             geometry at the top or bottom edge of the focused workspace.
 *
 */
#include "all.h"

/* X11 window id of the most recently hidden dropdown. `dropdown recall`
 * re-opens that window on the focused workspace. */
static xcb_window_t last_hidden_dropdown_window = XCB_NONE;

/*
 * Returns the client (leaf) container for the given container, descending the
 * focus stack as far as possible.
 *
 */
static Con *dropdown_client(Con *con) {
    if (con == NULL) {
        return NULL;
    }

    Con *client = con_descend_focused(con);
    if (client == NULL || client->window == NULL) {
        return NULL;
    }

    return client;
}

/*
 * Ensures that the given client container is floating and returns the
 * surrounding CT_FLOATING_CON.
 *
 */
static Con *dropdown_floating(Con *client) {
    Con *floating = con_inside_floating(client);
    if (floating == NULL) {
        floating_enable(client, false);
        floating = con_inside_floating(client);
    }
    return floating;
}

/*
 * Whether the given dropdown client is currently visible, i.e. attached to the
 * same workspace as the focused container.
 *
 */
static bool dropdown_is_visible(Con *client) {
    Con *floating = con_inside_floating(client);
    if (floating == NULL) {
        return false;
    }

    Con *ws = con_get_workspace(floating);
    if (ws == NULL || con_is_internal(ws)) {
        return false;
    }

    return ws == con_get_workspace(focused);
}

/*
 * Strips leading/trailing whitespace from a spec field in place and returns
 * it.
 *
 */
static char *dropdown_trim(char *field) {
    while (*field == ' ' || *field == '\t') {
        field++;
    }
    char *end = field + strlen(field);
    while (end > field && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return field;
}

/*
 * Parses a `position,height,width,align` geometry spec and stores the result
 * as per-client overrides. Fields may be empty to keep the defaults. Invalid
 * fields are logged and ignored.
 *
 */
static void dropdown_apply_spec(Con *client, const char *spec) {
    client->dropdown_geometry_valid = true;
    client->dropdown_position = config.dropdown_position == DROPDOWN_POSITION_BOTTOM
                                    ? DROPDOWN_POSITION_BOTTOM
                                    : DROPDOWN_POSITION_TOP;
    client->dropdown_height = config.dropdown_height;
    client->dropdown_width = 1.0;
    client->dropdown_align = DROPDOWN_ALIGN_CENTER;

    char *copy = sstrdup(spec);
    char *saveptr = copy;
    char *field = NULL;
    int index = 0;
    /* strsep (unlike strtok) keeps empty fields, which are used to keep the
     * defaults for individual values (e.g. "bottom,,0.4,"). */
    while ((field = strsep(&saveptr, ",")) != NULL) {
        char *value = dropdown_trim(field);
        if (*value != '\0') {
            switch (index) {
                case 0:
                    if (strcasecmp(value, "top") == 0) {
                        client->dropdown_position = DROPDOWN_POSITION_TOP;
                    } else if (strcasecmp(value, "bottom") == 0) {
                        client->dropdown_position = DROPDOWN_POSITION_BOTTOM;
                    } else {
                        ELOG("dropdown: invalid position \"%s\" (expected top|bottom)\n", value);
                    }
                    break;
                case 1:
                case 2: {
                    char *end = NULL;
                    const double ratio = strtod(value, &end);
                    if (end == value || *end != '\0' || ratio < 0.05 || ratio > 1.0) {
                        ELOG("dropdown: invalid %s \"%s\" (expected 0.05..1.0)\n",
                             index == 1 ? "height" : "width", value);
                        break;
                    }
                    if (index == 1) {
                        client->dropdown_height = ratio;
                    } else {
                        client->dropdown_width = ratio;
                    }
                    break;
                }
                case 3:
                    if (strcasecmp(value, "left") == 0) {
                        client->dropdown_align = DROPDOWN_ALIGN_LEFT;
                    } else if (strcasecmp(value, "center") == 0) {
                        client->dropdown_align = DROPDOWN_ALIGN_CENTER;
                    } else if (strcasecmp(value, "right") == 0) {
                        client->dropdown_align = DROPDOWN_ALIGN_RIGHT;
                    } else {
                        ELOG("dropdown: invalid align \"%s\" (expected left|center|right)\n", value);
                    }
                    break;
                default:
                    ELOG("dropdown: ignoring extra geometry field \"%s\"\n", value);
                    break;
            }
        }
        index++;
    }

    free(copy);
}

/*
 * Returns the effective position (per-client override or global config).
 *
 */
static int dropdown_client_position(Con *client) {
    if (client != NULL && client->dropdown_geometry_valid) {
        return client->dropdown_position;
    }

    return config.dropdown_position == DROPDOWN_POSITION_BOTTOM
               ? DROPDOWN_POSITION_BOTTOM
               : DROPDOWN_POSITION_TOP;
}

/*
 * Computes the target geometry of a dropdown window on the given workspace.
 * The workspace rect already has the outer gaps applied.
 *
 */
static Rect dropdown_geometry(Con *client, Con *ws) {
    int position = dropdown_client_position(client);
    double height_ratio = config.dropdown_height;
    double width_ratio = 1.0;
    int align = DROPDOWN_ALIGN_CENTER;

    if (client != NULL && client->dropdown_geometry_valid) {
        height_ratio = client->dropdown_height;
        width_ratio = client->dropdown_width;
        align = client->dropdown_align;
    }

    uint32_t width = (uint32_t)((double)ws->rect.width * width_ratio);
    uint32_t height = (uint32_t)((double)ws->rect.height * height_ratio);
    if (width < 1) {
        width = 1;
    }
    if (width > ws->rect.width) {
        width = ws->rect.width;
    }
    if (height < 1) {
        height = 1;
    }
    if (height > ws->rect.height) {
        height = ws->rect.height;
    }

    Rect rect = ws->rect;
    switch (align) {
        case DROPDOWN_ALIGN_LEFT:
            rect.x = ws->rect.x;
            break;
        case DROPDOWN_ALIGN_RIGHT:
            rect.x = ws->rect.x + ws->rect.width - width;
            break;
        case DROPDOWN_ALIGN_CENTER:
        default:
            rect.x = ws->rect.x + (ws->rect.width - width) / 2;
            break;
    }

    if (position == DROPDOWN_POSITION_BOTTOM) {
        rect.y = ws->rect.y + ws->rect.height - height;
    } else {
        rect.y = ws->rect.y;
    }

    rect.width = width;
    rect.height = height;
    return rect;
}

/*
 * Returns the off-screen rectangle above (top) or below (bottom) the given
 * target rectangle.
 *
 */
static anim_rect dropdown_offscreen(anim_rect target, int position) {
    const int margin = logical_px(10);
    anim_rect offscreen = target;
    if (position == DROPDOWN_POSITION_BOTTOM) {
        offscreen.y = target.y + target.height + margin;
    } else {
        offscreen.y = target.y - target.height - margin;
    }
    return offscreen;
}

/*
 * Shows the given window as a dropdown panel on the focused workspace.
 *
 */
bool dropdown_show(Con *con, const char *spec) {
    Con *client = dropdown_client(con);
    if (client == NULL) {
        return false;
    }

    /* Apply per-invocation geometry before anything is moved or animated. */
    if (spec != NULL) {
        dropdown_apply_spec(client, spec);
    }

    Con *active = con_get_workspace(focused);
    if (active == NULL || con_is_internal(active)) {
        DLOG("dropdown_show: no active workspace\n");
        return false;
    }

    Con *floating = dropdown_floating(client);
    if (floating == NULL) {
        return false;
    }

    Con *current = con_get_workspace(floating);
    const bool was_on_active_workspace = (current == active);
    const Rect old_floating_rect = floating->rect;
    const Rect old_client_rect = client->rect;

    if (current == workspace_get("__i3_scratch")) {
        DLOG("dropdown_show: showing client %p from scratchpad\n", client);
        scratchpad_show(client);
    } else if (current != active) {
        DLOG("dropdown_show: moving client %p to workspace %s\n", client, active->name);
        con_move_to_workspace(floating, active, true, false, false);
        workspace_show(active);
        con_activate(client);
    } else {
        con_activate(client);
    }

    /* scratchpad_show() centers and resizes fresh scratchpad windows, so the
     * dropdown geometry is applied afterwards. */
    const Rect target_rect = dropdown_geometry(client, active);
    const bool already_at_target =
        was_on_active_workspace && rect_equals(old_floating_rect, target_rect);
    floating->rect = target_rect;
    floating_check_size(floating, false);
    render_con(floating);

    /* Keep the dropdown geometry when the window is later shown through the
     * regular scratchpad commands. */
    floating->scratchpad_state = SCRATCHPAD_CHANGED;

    client->dropdown = true;
    client->anim.dropdown_hide_pending = false;
    /* Prevent x_push_node() from starting its generic open animation; the
     * dropdown slide is configured below. */
    client->anim.ever_pushed = true;
    if (client->window != NULL &&
        last_hidden_dropdown_window == client->window->id) {
        /* This was the last hidden dropdown; it is visible again. */
        last_hidden_dropdown_window = XCB_NONE;
    }

    if (!already_at_target) {
        const anim_rect target = animation_rect_from_layout(client->rect);
        anim_rect from;
        if (client->anim.active) {
            /* Reverse an in-flight show/hide from the currently visible
             * position instead of jumping back to the screen edge. */
            from = client->anim.cur;
        } else if (was_on_active_workspace) {
            /* The panel is already on this workspace; just move it to the
             * dropdown geometry. */
            from = animation_rect_from_layout(old_client_rect);
        } else {
            from = dropdown_offscreen(target, dropdown_client_position(client));
        }

        animation_begin(client, from, target,
                        config.animation_dropdown_curve,
                        config.animation_dropdown_duration / 1000.0, false);
    }

    return true;
}

/*
 * Hides the given dropdown window into the scratchpad, optionally animating it
 * off-screen first.
 *
 */
bool dropdown_hide(Con *con) {
    Con *client = dropdown_client(con);
    if (client == NULL || !con_is_dropdown(client)) {
        return false;
    }

    Con *floating = con_inside_floating(client);
    if (floating == NULL || !dropdown_is_visible(client)) {
        DLOG("dropdown_hide: window is not a visible dropdown\n");
        return false;
    }

    if (client->window != NULL) {
        last_hidden_dropdown_window = client->window->id;
    }

    const double duration = config.animation_dropdown_duration / 1000.0;
    const int position = dropdown_client_position(client);
    if (config.animations && duration > 0.0) {
        const anim_rect from = client->anim.active
                                   ? client->anim.cur
                                   : animation_rect_from_layout(client->rect);
        client->anim.dropdown_hide_pending = true;
        animation_begin(client, from, dropdown_offscreen(from, position),
                        config.animation_dropdown_curve, duration, false);
        if (!client->anim.active) {
            /* animation_begin() refused to start (e.g. equal rects). */
            client->anim.dropdown_hide_pending = false;
            scratchpad_move(client);
        }
    } else {
        scratchpad_move(client);
    }

    return true;
}

/*
 * Toggles the given window between the visible dropdown and the scratchpad.
 *
 */
bool dropdown_toggle(Con *con, const char *spec) {
    Con *client = dropdown_client(con);
    if (client == NULL) {
        return false;
    }

    if (con_is_dropdown(client) && dropdown_is_visible(client)) {
        return dropdown_hide(client);
    }

    return dropdown_show(client, spec);
}

bool con_is_dropdown(Con *con) {
    if (con == NULL) {
        return false;
    }

    if (con->dropdown) {
        return true;
    }

    Con *floating = con_inside_floating(con);
    if (floating == NULL) {
        return false;
    }

    Con *client = con_descend_focused(floating);
    return client != NULL && client->dropdown;
}

/*
 * Returns the most recently hidden dropdown if it still exists.
 *
 */
static Con *last_hidden_dropdown(void) {
    if (last_hidden_dropdown_window == XCB_NONE) {
        return NULL;
    }

    Con *con = con_by_window_id(last_hidden_dropdown_window);
    if (con == NULL || con->window == NULL) {
        last_hidden_dropdown_window = XCB_NONE;
        return NULL;
    }
    return con;
}

/*
 * Plain `dropdown toggle`: acts on the focused window. A normal window is
 * turned into a dropdown, an already visible dropdown is hidden. Hidden
 * dropdowns are brought back with `dropdown recall` (or criteria).
 *
 */
bool dropdown_toggle_global(const char *spec) {
    Con *client = dropdown_client(focused);
    if (client == NULL) {
        return false;
    }

    if (con_is_dropdown(client) && dropdown_is_visible(client)) {
        return dropdown_hide(client);
    }

    return dropdown_show(client, spec);
}

bool dropdown_show_global(const char *spec) {
    Con *client = dropdown_client(focused);
    if (client == NULL) {
        return false;
    }

    return dropdown_show(client, spec);
}

bool dropdown_hide_global(void) {
    Con *client = dropdown_client(focused);
    if (client == NULL || !con_is_dropdown(client) || !dropdown_is_visible(client)) {
        return false;
    }

    return dropdown_hide(client);
}

/*
 * Re-opens the most recently hidden dropdown on the focused workspace.
 *
 */
bool dropdown_recall(const char *spec) {
    Con *last = last_hidden_dropdown();
    if (last == NULL) {
        return false;
    }

    return dropdown_show(last, spec);
}
