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
 * Returns the margin used around the dropdown area and between tiled
 * dropdowns. It reuses the workspace inner gap so that dropdowns line up with
 * the tiled windows, capped at 16 pixels.
 *
 */
static int dropdown_gap(Con *ws) {
    int gap = calculate_effective_gaps(ws).inner;
    if (gap < 0) {
        return 0;
    }
    if (gap > 16) {
        return 16;
    }
    return gap;
}

/*
 * Returns the workspace rect inset by dropdown_gap(ws) on all four sides. The
 * single-dropdown geometry and the shared multi-dropdown band both use this
 * area, so they always keep the same outer margin.
 *
 */
static Rect dropdown_area(Con *ws) {
    const int margin = dropdown_gap(ws);
    Rect area = {
        .x = ws->rect.x + margin,
        .y = ws->rect.y + margin,
        .width = (int)ws->rect.width - 2 * margin,
        .height = (int)ws->rect.height - 2 * margin,
    };
    if (area.width < 1) {
        area.width = 1;
    }
    if (area.height < 1) {
        area.height = 1;
    }
    return area;
}

/*
 * Computes the target geometry of a dropdown window on the given workspace.
 * The workspace rect already has the outer gaps applied; the dropdown is
 * additionally inset by dropdown_gap(ws) so that it does not touch the screen
 * edge.
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

    const Rect area = dropdown_area(ws);
    uint32_t width = (uint32_t)((double)area.width * width_ratio);
    uint32_t height = (uint32_t)((double)area.height * height_ratio);
    if (width < 1) {
        width = 1;
    }
    if (width > area.width) {
        width = area.width;
    }
    if (height < 1) {
        height = 1;
    }
    if (height > area.height) {
        height = area.height;
    }

    Rect rect = area;
    switch (align) {
        case DROPDOWN_ALIGN_LEFT:
            rect.x = area.x;
            break;
        case DROPDOWN_ALIGN_RIGHT:
            rect.x = area.x + area.width - width;
            break;
        case DROPDOWN_ALIGN_CENTER:
        default:
            rect.x = area.x + (area.width - width) / 2;
            break;
    }

    if (position == DROPDOWN_POSITION_BOTTOM) {
        rect.y = area.y + area.height - height;
    } else {
        rect.y = area.y;
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

/* Maximum number of dropdowns that are laid out at the same time. */
#define DROPDOWN_MAX_VISIBLE 32

/*
 * Collects the visible dropdown clients on the given workspace in a stable
 * order (all_cons preserves creation order). `exclude` is skipped so that a
 * dropdown which is about to be hidden does not take part in the reflow.
 *
 */
static int dropdown_collect_visible(Con *ws, Con *exclude, Con **clients, int max) {
    int count = 0;
    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        if (con->type != CT_CON || con->window == NULL || !con->dropdown) {
            continue;
        }
        /* A dropdown that is already sliding out must finish hiding; re-tiling
         * it here would cancel the hide animation but leave the pending flag
         * set, which would move it to the scratchpad later. */
        if (con->anim.dropdown_hide_pending) {
            continue;
        }
        if (con == exclude) {
            continue;
        }
        Con *floating = con_inside_floating(con);
        if (floating == NULL || con_get_workspace(floating) != ws) {
            continue;
        }
        if (count >= max) {
            ELOG("dropdown: more than %d visible dropdowns, ignoring the rest\n", max);
            break;
        }
        clients[count++] = con;
    }
    return count;
}

/*
 * The band shared by all dropdowns on a workspace. It is inset by
 * dropdown_gap(ws) like the single-dropdown geometry, its height is derived
 * from the global dropdown_height and its vertical position from the global
 * dropdown_position: per-client overrides do not make sense for a shared band.
 *
 */
static Rect dropdown_band(Con *ws) {
    const Rect area = dropdown_area(ws);
    uint32_t height = (uint32_t)((double)area.height * config.dropdown_height);
    if (height < 1) {
        height = 1;
    }
    if (height > area.height) {
        height = area.height;
    }

    Rect band = area;
    band.height = height;
    if (config.dropdown_position == DROPDOWN_POSITION_BOTTOM) {
        band.y = area.y + area.height - height;
    } else {
        band.y = area.y;
    }
    return band;
}

/*
 * Returns the slot of the index-th dropdown inside the band. Up to four
 * dropdowns are tiled in one row; more use two rows.
 *
 */
static Rect dropdown_slot(Rect band, int index, int count, int gap) {
    int rows = 1;
    int cols = count;
    if (count > 4) {
        rows = 2;
        cols = (count + 1) / 2;
    }

    int cell_w = ((int)band.width - (cols - 1) * gap) / cols;
    int cell_h = ((int)band.height - (rows - 1) * gap) / rows;
    if (cell_w < 1) {
        cell_w = 1;
    }
    if (cell_h < 1) {
        cell_h = 1;
    }

    const int row = index / cols;
    const int col = index % cols;
    return (Rect){
        .x = band.x + col * (cell_w + gap),
        .y = band.y + row * (cell_h + gap),
        .width = cell_w,
        .height = cell_h,
    };
}

/*
 * Lays out all visible dropdowns on `ws` so that they do not overlap and
 * animates them into their new slots. This only runs when a dropdown is shown
 * or hidden; there is no per-frame work.
 *
 * With a single dropdown the per-client geometry is used; with multiple
 * dropdowns they share the global band and are tiled as described above.
 *
 */
static void dropdown_layout_visible(Con *ws, Con *newly_shown, bool newly_shown_was_hidden, Con *exclude) {
    Con *clients[DROPDOWN_MAX_VISIBLE];
    const int count = dropdown_collect_visible(ws, exclude, clients, DROPDOWN_MAX_VISIBLE);
    if (count == 0) {
        return;
    }

    const bool multiple = (count > 1);
    Rect band = dropdown_band(ws);
    const int gap = dropdown_gap(ws);
    const int band_position = config.dropdown_position == DROPDOWN_POSITION_BOTTOM
                                  ? DROPDOWN_POSITION_BOTTOM
                                  : DROPDOWN_POSITION_TOP;

    for (int i = 0; i < count; i++) {
        Con *client = clients[i];
        Con *floating = con_inside_floating(client);
        if (floating == NULL) {
            continue;
        }

        const Rect old_client_rect = client->rect;
        Rect target;
        int position;
        if (multiple) {
            target = dropdown_slot(band, i, count, gap);
            position = band_position;
        } else {
            target = dropdown_geometry(client, ws);
            position = dropdown_client_position(client);
        }

        floating->rect = target;
        /* With several dropdowns every tile must get the exact slot size;
         * honoring size hints here would make the tiles uneven. */
        if (!multiple) {
            floating_check_size(floating, false);
        }
        render_con(floating);

        const Rect new_client_rect = client->rect;
        if (rect_equals(new_client_rect, old_client_rect) && !client->anim.active) {
            continue;
        }

        anim_rect from;
        if (client == newly_shown && newly_shown_was_hidden) {
            from = dropdown_offscreen(animation_rect_from_layout(new_client_rect), position);
        } else if (client->anim.active) {
            from = client->anim.cur;
        } else {
            from = animation_rect_from_layout(old_client_rect);
        }

        animation_begin(client, from, animation_rect_from_layout(new_client_rect),
                        config.animation_dropdown_curve,
                        config.animation_dropdown_duration / 1000.0, false);
    }
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
    const bool was_hidden = (current == workspace_get("__i3_scratch"));

    if (was_hidden) {
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

    /* Keep the dropdown geometry when the window is later shown through the
     * regular scratchpad commands. */
    floating->scratchpad_state = SCRATCHPAD_CHANGED;

    client->dropdown = true;
    client->anim.dropdown_hide_pending = false;
    /* Prevent x_push_node() from starting its generic open animation; the
     * dropdown slide is configured in dropdown_layout_visible(). */
    client->anim.ever_pushed = true;
    if (client->window != NULL &&
        last_hidden_dropdown_window == client->window->id) {
        /* This was the last hidden dropdown; it is visible again. */
        last_hidden_dropdown_window = XCB_NONE;
    }

    /* Lay out all dropdowns on the workspace, including this one. A window
     * which was hidden in the scratchpad slides in from the screen edge; the
     * already visible dropdowns reflow into their new slots. */
    dropdown_layout_visible(active, client, was_hidden, NULL);

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
    if (floating == NULL) {
        DLOG("dropdown_hide: window is not floating\n");
        return false;
    }

    Con *current = con_get_workspace(floating);
    if (current == NULL || con_is_internal(current)) {
        DLOG("dropdown_hide: window is not a visible dropdown\n");
        return false;
    }

    Con *active = con_get_workspace(focused);
    if (active != NULL && !con_is_internal(active) && current == active) {
        /* Reflow the remaining dropdowns into their new slots before this one
         * slides away; the hidden client is excluded from the layout. */
        dropdown_layout_visible(active, NULL, false, client);
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
