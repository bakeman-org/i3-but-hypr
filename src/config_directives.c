/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * config_directives.c: all config storing functions (see config_parser.c)
 *
 */
#include "all.h"

#include <wordexp.h>

/*******************************************************************************
 * Include functions.
 ******************************************************************************/

CFGFUN(include, const char *pattern) {
    DLOG("include %s\n", pattern);

    wordexp_t p;
    const int ret = wordexp(pattern, &p, 0);
    if (ret != 0) {
        ELOG("wordexp(%s): error %d\n", pattern, ret);
        result->has_errors = true;
        return;
    }
    char **w = p.we_wordv;
    for (size_t i = 0; i < p.we_wordc; i++) {
        char resolved_path[PATH_MAX] = {'\0'};
        if (realpath(w[i], resolved_path) == NULL) {
            LOG("Skipping %s: %s\n", w[i], strerror(errno));
            continue;
        }

        bool skip = false;
        IncludedFile *file;
        TAILQ_FOREACH (file, &included_files, files) {
            if (strcmp(file->path, resolved_path) == 0) {
                skip = true;
                break;
            }
        }
        if (skip) {
            LOG("Skipping file %s (already included)\n", resolved_path);
            continue;
        }

        LOG("Including config file %s\n", resolved_path);

        file = scalloc(1, sizeof(IncludedFile));
        file->path = sstrdup(resolved_path);
        TAILQ_INSERT_TAIL(&included_files, file, files);

        struct stack stack = {0};
        struct parser_ctx ctx = {
            .use_nagbar = result->ctx->use_nagbar,
            .stack = &stack,
            .variables = result->ctx->variables,
        };
        switch (parse_file(&ctx, resolved_path, file)) {
            case PARSE_FILE_SUCCESS:
                break;

            case PARSE_FILE_FAILED:
                ELOG("including config file %s: %s\n", resolved_path, strerror(errno));
                /* fallthrough */

            case PARSE_FILE_CONFIG_ERRORS:
                result->has_errors = true;
                TAILQ_REMOVE(&included_files, file, files);
                FREE(file->path);
                FREE(file->raw_contents);
                FREE(file->variable_replaced_contents);
                FREE(file);
                break;

            default:
                /* missing case statement */
                assert(false);
        }
        result->ctx->variables = ctx.variables; /* In case head was modified */
    }
    wordfree(&p);
}

/*******************************************************************************
 * Criteria functions.
 ******************************************************************************/

static int criteria_next_state;

/*
 * Initializes the specified 'Match' data structure and the initial state of
 * commands.c for matching target windows of a command.
 *
 */
CFGFUN(criteria_init, const int _state) {
    criteria_next_state = _state;

    DLOG("Initializing criteria, current_match = %p, state = %d\n", current_match, _state);
    match_free(current_match);
    match_init(current_match);
}

CFGFUN(criteria_pop_state) {
    result->next_state = criteria_next_state;
}

/*
 * Interprets a ctype=cvalue pair and adds it to the current match
 * specification.
 *
 */
CFGFUN(criteria_add, const char *ctype, const char *cvalue) {
    match_parse_property(current_match, ctype, cvalue);
}

/*******************************************************************************
 * Utility functions
 ******************************************************************************/

/*
 * A utility function to convert a string containing the group and modifiers to
 * the corresponding bit mask.
 */
i3_event_state_mask_t event_state_from_str(const char *str) {
    /* It might be better to use strtok() here, but the simpler strstr() should
     * do for now. */
    i3_event_state_mask_t result = 0;
    if (str == NULL) {
        return result;
    }
    if (strstr(str, "Mod1") != NULL) {
        result |= XCB_KEY_BUT_MASK_MOD_1;
    }
    if (strstr(str, "Mod2") != NULL) {
        result |= XCB_KEY_BUT_MASK_MOD_2;
    }
    if (strstr(str, "Mod3") != NULL) {
        result |= XCB_KEY_BUT_MASK_MOD_3;
    }
    if (strstr(str, "Mod4") != NULL) {
        result |= XCB_KEY_BUT_MASK_MOD_4;
    }
    if (strstr(str, "Mod5") != NULL) {
        result |= XCB_KEY_BUT_MASK_MOD_5;
    }
    if (strstr(str, "Control") != NULL ||
        strstr(str, "Ctrl") != NULL) {
        result |= XCB_KEY_BUT_MASK_CONTROL;
    }
    if (strstr(str, "Shift") != NULL) {
        result |= XCB_KEY_BUT_MASK_SHIFT;
    }

    if (strstr(str, "Group1") != NULL) {
        result |= (I3_XKB_GROUP_MASK_1 << 16);
    }
    if (strstr(str, "Group2") != NULL ||
        strstr(str, "Mode_switch") != NULL) {
        result |= (I3_XKB_GROUP_MASK_2 << 16);
    }
    if (strstr(str, "Group3") != NULL) {
        result |= (I3_XKB_GROUP_MASK_3 << 16);
    }
    if (strstr(str, "Group4") != NULL) {
        result |= (I3_XKB_GROUP_MASK_4 << 16);
    }
    return result;
}

CFGFUN(font, const char *font) {
    config.font = load_font(font, true);
    set_font(&config.font);
}

CFGFUN(titlebar_font, const char *font) {
    /* load_font() implicitly frees the currently *saved* font, which is
     * the global font while parsing — freeing it here would leave
     * config.font dangling. Temporarily redirect the free to the titlebar
     * font this directive replaces, then restore the global font. */
    set_font(&config.titlebar_font);
    config.titlebar_font = load_font(font, true);
    set_font(&config.font);
}

CFGFUN(binding, const char *bindtype, const char *modifiers, const char *key, const char *release, const char *border, const char *whole_window, const char *exclude_titlebar, const char *command) {
    configure_binding(bindtype, modifiers, key, release, border, whole_window, exclude_titlebar, command, DEFAULT_BINDING_MODE, false);
}

/*******************************************************************************
 * Mode handling
 ******************************************************************************/

static char *current_mode;
static bool current_mode_pango_markup;

CFGFUN(mode_binding, const char *bindtype, const char *modifiers, const char *key, const char *release, const char *border, const char *whole_window, const char *exclude_titlebar, const char *command) {
    if (current_mode == NULL) {
        /* When using an invalid mode name, e.g. “default” */
        return;
    }

    configure_binding(bindtype, modifiers, key, release, border, whole_window, exclude_titlebar, command, current_mode, current_mode_pango_markup);
}

CFGFUN(enter_mode, const char *pango_markup, const char *modename) {
    if (strcmp(modename, DEFAULT_BINDING_MODE) == 0) {
        ELOG("You cannot use the name %s for your mode\n", DEFAULT_BINDING_MODE);
        return;
    }

    struct Mode *mode;
    SLIST_FOREACH (mode, &modes, modes) {
        if (strcmp(mode->name, modename) == 0) {
            ELOG("The binding mode with name \"%s\" is defined at least twice.\n", modename);
        }
    }

    DLOG("\t now in mode %s\n", modename);
    FREE(current_mode);
    current_mode = sstrdup(modename);
    current_mode_pango_markup = (pango_markup != NULL);
}

CFGFUN(exec, const char *exectype, const char *no_startup_id, const char *command) {
    struct Autostart *new = smalloc(sizeof(struct Autostart));
    new->command = sstrdup(command);
    new->no_startup_id = (no_startup_id != NULL);
    if (strcmp(exectype, "exec") == 0) {
        TAILQ_INSERT_TAIL(&autostarts, new, autostarts);
    } else {
        TAILQ_INSERT_TAIL(&autostarts_always, new, autostarts_always);
    }
}

CFGFUN(for_window, const char *command) {
    if (current_match->error != NULL) {
        ELOG("match has error: %s\n", current_match->error);
        return;
    }
    if (match_is_empty(current_match)) {
        ELOG("Match is empty, ignoring this for_window statement\n");
        return;
    }
    DLOG("\t should execute command %s for the criteria mentioned above\n", command);
    Assignment *assignment = scalloc(1, sizeof(Assignment));
    assignment->type = A_COMMAND;
    match_copy(&(assignment->match), current_match);
    assignment->dest.command = sstrdup(command);
    TAILQ_INSERT_TAIL(&assignments, assignment, assignments);
}

static void apply_gaps(gaps_t *gaps, const gaps_mask_t mask, const int value) {
    if (gaps == NULL) {
        return;
    }
    if (mask & GAPS_INNER) {
        gaps->inner = value;
    }
    if (mask & GAPS_TOP) {
        gaps->top = value;
    }
    if (mask & GAPS_RIGHT) {
        gaps->right = value;
    }
    if (mask & GAPS_BOTTOM) {
        gaps->bottom = value;
    }
    if (mask & GAPS_LEFT) {
        gaps->left = value;
    }
}

static void create_gaps_assignment(const char *workspace, const gaps_mask_t mask, const int pixels) {
    if (mask == 0) {
        return;
    }

    DLOG("Setting gaps for workspace %s", workspace);

    bool found = false;
    struct Workspace_Assignment *assignment;
    TAILQ_FOREACH (assignment, &ws_assignments, ws_assignments) {
        if (strcasecmp(assignment->name, workspace) == 0) {
            found = true;
            break;
        }
    }

    /* Assignment does not yet exist, let's create it. */
    if (!found) {
        assignment = scalloc(1, sizeof(struct Workspace_Assignment));
        assignment->name = sstrdup(workspace);
        assignment->output = NULL;
        TAILQ_INSERT_TAIL(&ws_assignments, assignment, ws_assignments);
    }

    assignment->gaps_mask |= mask;
    apply_gaps(&assignment->gaps, mask, pixels);
}

static gaps_mask_t gaps_scope_to_mask(const char *scope) {
    if (!strcmp(scope, "inner")) {
        return GAPS_INNER;
    }
    if (!strcmp(scope, "outer")) {
        return GAPS_OUTER;
    }
    if (!strcmp(scope, "vertical")) {
        return GAPS_VERTICAL;
    }
    if (!strcmp(scope, "horizontal")) {
        return GAPS_HORIZONTAL;
    }
    if (!strcmp(scope, "top")) {
        return GAPS_TOP;
    }
    if (!strcmp(scope, "right")) {
        return GAPS_RIGHT;
    }
    if (!strcmp(scope, "bottom")) {
        return GAPS_BOTTOM;
    }
    if (!strcmp(scope, "left")) {
        return GAPS_LEFT;
    }
    ELOG("Invalid command, cannot process scope %s", scope);
    return 0;
}

CFGFUN(gaps, const char *workspace, const char *scope, const long value) {
    const int pixels = logical_px(value);
    const gaps_mask_t mask = gaps_scope_to_mask(scope);

    if (workspace == NULL) {
        apply_gaps(&config.gaps, mask, pixels);
    } else {
        create_gaps_assignment(workspace, mask, pixels);
    }
}

CFGFUN(smart_borders, const char *enable) {
    if (!strcmp(enable, "no_gaps")) {
        config.hide_edge_borders = HEBM_SMART_NO_GAPS;
    } else if (boolstr(enable)) {
        if (config.hide_edge_borders == HEBM_NONE) {
            /* Only enable this if hide_edge_borders is at the default value as it otherwise takes precedence */
            config.hide_edge_borders = HEBM_SMART;
        } else {
            ELOG("Both hide_edge_borders and smart_borders was used. "
                 "Ignoring smart_borders as it is deprecated.\n");
        }
    }
}

CFGFUN(smart_gaps, const char *enable) {
    if (!strcmp(enable, "inverse_outer")) {
        config.smart_gaps = SMART_GAPS_INVERSE_OUTER;
    } else {
        config.smart_gaps = boolstr(enable) ? SMART_GAPS_ON : SMART_GAPS_OFF;
    }
}

CFGFUN(floating_minimum_size, const long width, const long height) {
    config.floating_minimum_width = width;
    config.floating_minimum_height = height;
}

CFGFUN(floating_maximum_size, const long width, const long height) {
    config.floating_maximum_width = width;
    config.floating_maximum_height = height;
}

CFGFUN(floating_modifier, const char *modifiers) {
    config.floating_modifier = event_state_from_str(modifiers);
}

CFGFUN(tiling_drag_swap_modifier, const char *modifiers) {
    config.swap_modifier = event_state_from_str(modifiers);
}

CFGFUN(default_orientation, const char *orientation) {
    if (strcmp(orientation, "horizontal") == 0) {
        config.default_orientation = HORIZ;
    } else if (strcmp(orientation, "vertical") == 0) {
        config.default_orientation = VERT;
    } else {
        config.default_orientation = NO_ORIENTATION;
    }
}

CFGFUN(workspace_layout, const char *layout) {
    if (!layout_from_name(layout, &config.default_layout)) {
        ELOG("Unknown workspace layout \"%s\", falling back to default\n", layout);
        config.default_layout = L_DEFAULT;
    }
}

CFGFUN(default_border, const char *windowtype, const char *border, const long width) {
    int border_style;
    int border_width;

    if (strcmp(border, "1pixel") == 0) {
        border_style = BS_PIXEL;
        border_width = 1;
    } else if (strcmp(border, "none") == 0) {
        border_style = BS_NONE;
        border_width = 0;
    } else if (strcmp(border, "pixel") == 0) {
        border_style = BS_PIXEL;
        border_width = width;
    } else {
        border_style = BS_NORMAL;
        border_width = width;
    }

    if ((strcmp(windowtype, "default_border") == 0) ||
        (strcmp(windowtype, "new_window") == 0)) {
        DLOG("default tiled border style = %d and border width = %d (%d physical px)\n",
             border_style, border_width, logical_px(border_width));
        config.default_border = border_style;
        config.default_border_width = logical_px(border_width);
    } else {
        DLOG("default floating border style = %d and border width = %d (%d physical px)\n",
             border_style, border_width, logical_px(border_width));
        config.default_floating_border = border_style;
        config.default_floating_border_width = logical_px(border_width);
    }
}

CFGFUN(hide_edge_borders, const char *borders) {
    if (strcmp(borders, "smart_no_gaps") == 0) {
        config.hide_edge_borders = HEBM_SMART_NO_GAPS;
    } else if (strcmp(borders, "smart") == 0) {
        config.hide_edge_borders = HEBM_SMART;
    } else if (strcmp(borders, "vertical") == 0) {
        config.hide_edge_borders = HEBM_VERTICAL;
    } else if (strcmp(borders, "horizontal") == 0) {
        config.hide_edge_borders = HEBM_HORIZONTAL;
    } else if (strcmp(borders, "both") == 0) {
        config.hide_edge_borders = HEBM_BOTH;
    } else if (strcmp(borders, "none") == 0) {
        config.hide_edge_borders = HEBM_NONE;
    } else if (boolstr(borders)) {
        config.hide_edge_borders = HEBM_VERTICAL;
    } else {
        config.hide_edge_borders = HEBM_NONE;
    }
}

CFGFUN(focus_follows_mouse, const char *value) {
    config.disable_focus_follows_mouse = !boolstr(value);
}

CFGFUN(mouse_warping, const char *value) {
    if (strcmp(value, "none") == 0) {
        config.mouse_warping = POINTER_WARPING_NONE;
    } else if (strcmp(value, "output") == 0) {
        config.mouse_warping = POINTER_WARPING_OUTPUT;
    }
}

CFGFUN(force_xinerama, const char *value) {
    config.force_xinerama = boolstr(value);
}

CFGFUN(disable_randr15, const char *value) {
    config.disable_randr15 = boolstr(value);
}

CFGFUN(focus_wrapping, const char *value) {
    if (strcmp(value, "force") == 0) {
        config.focus_wrapping = FOCUS_WRAPPING_FORCE;
    } else if (strcmp(value, "workspace") == 0) {
        config.focus_wrapping = FOCUS_WRAPPING_WORKSPACE;
    } else if (boolstr(value)) {
        config.focus_wrapping = FOCUS_WRAPPING_ON;
    } else {
        config.focus_wrapping = FOCUS_WRAPPING_OFF;
    }
}

CFGFUN(force_focus_wrapping, const char *value) {
    /* Legacy syntax. */
    if (boolstr(value)) {
        config.focus_wrapping = FOCUS_WRAPPING_FORCE;
    } else {
        /* For "force_focus_wrapping off", don't enable or disable
         * focus wrapping, just ensure it's not forced. */
        if (config.focus_wrapping == FOCUS_WRAPPING_FORCE) {
            config.focus_wrapping = FOCUS_WRAPPING_ON;
        }
    }
}

CFGFUN(workspace_back_and_forth, const char *value) {
    config.workspace_auto_back_and_forth = boolstr(value);
}

CFGFUN(fake_outputs, const char *outputs) {
    free(config.fake_outputs);
    config.fake_outputs = sstrdup(outputs);
}

CFGFUN(force_display_urgency_hint, const long duration_ms) {
    config.workspace_urgency_timer = duration_ms / 1000.0;
}

CFGFUN(focus_on_window_activation, const char *mode) {
    if (strcmp(mode, "smart") == 0) {
        config.focus_on_window_activation = FOWA_SMART;
    } else if (strcmp(mode, "urgent") == 0) {
        config.focus_on_window_activation = FOWA_URGENT;
    } else if (strcmp(mode, "focus") == 0) {
        config.focus_on_window_activation = FOWA_FOCUS;
    } else if (strcmp(mode, "none") == 0) {
        config.focus_on_window_activation = FOWA_NONE;
    } else {
        ELOG("Unknown focus_on_window_activation mode \"%s\", ignoring it.\n", mode);
        return;
    }

    DLOG("Set new focus_on_window_activation mode = %i.\n", config.focus_on_window_activation);
}

CFGFUN(title_align, const char *alignment) {
    if (strcmp(alignment, "left") == 0) {
        config.title_align = ALIGN_LEFT;
    } else if (strcmp(alignment, "center") == 0) {
        config.title_align = ALIGN_CENTER;
    } else if (strcmp(alignment, "right") == 0) {
        config.title_align = ALIGN_RIGHT;
    } else {
        assert(false);
    }
}

CFGFUN(show_marks, const char *value) {
    config.show_marks = boolstr(value);
}

CFGFUN(hot_reload, const char *value) {
    config.hot_reload = boolstr(value);
    /* Apply the new setting right away when hot reload is toggled via a
     * configuration reload; during the initial load the watcher is not
     * running yet and config_watcher_init() picks this up. */
    config_watcher_rearm();
}

static char *current_workspace = NULL;

CFGFUN(workspace, const char *workspace, const char *output) {
    struct Workspace_Assignment *assignment;

    /* When a new workspace line is encountered, for the first output word,
     * $workspace from the config.spec is non-NULL. Afterward, the parser calls
     * parser_clear_stack() because of the call line. Thus, we have to preserve
     * the workspace string. */
    if (workspace) {
        FREE(current_workspace);

        TAILQ_FOREACH (assignment, &ws_assignments, ws_assignments) {
            if (strcasecmp(assignment->name, workspace) == 0) {
                if (assignment->output != NULL) {
                    ELOG("You have a duplicate workspace assignment for workspace \"%s\"\n",
                         workspace);
                    return;
                }
            }
        }

        current_workspace = sstrdup(workspace);
    } else {
        if (!current_workspace) {
            DLOG("Both workspace and current_workspace are NULL, assuming we had an error before\n");
            return;
        }
        workspace = current_workspace;
    }

    DLOG("Assigning workspace \"%s\" to output \"%s\"\n", workspace, output);

    assignment = scalloc(1, sizeof(struct Workspace_Assignment));
    assignment->name = sstrdup(workspace);
    assignment->output = sstrdup(output);
    TAILQ_INSERT_TAIL(&ws_assignments, assignment, ws_assignments);
}

CFGFUN(ipc_socket, const char *path) {
    free(config.ipc_socket_path);
    config.ipc_socket_path = sstrdup(path);
}

CFGFUN(restart_state, const char *path) {
    free(config.restart_state_path);
    config.restart_state_path = sstrdup(path);
}

CFGFUN(popup_during_fullscreen, const char *value) {
    if (strcmp(value, "ignore") == 0) {
        config.popup_during_fullscreen = PDF_IGNORE;
    } else if (strcmp(value, "leave_fullscreen") == 0) {
        config.popup_during_fullscreen = PDF_LEAVE_FULLSCREEN;
    } else if (strcmp(value, "all") == 0) {
        config.popup_during_fullscreen = PDF_ALL;
    } else {
        config.popup_during_fullscreen = PDF_SMART;
    }
}

CFGFUN(color_single, const char *colorclass, const char *color) {
    /* used for client.background only currently */
    config.client.background = draw_util_hex_to_color(color);
}

CFGFUN(color, const char *colorclass, const char *border, const char *background, const char *text, const char *indicator, const char *child_border) {
#define APPLY_COLORS(classname)                                                                              \
    do {                                                                                                     \
        if (strcmp(colorclass, "client." #classname) == 0) {                                                 \
            if (strcmp("focused_tab_title", #classname) == 0) {                                              \
                config.client.got_focused_tab_title = true;                                                  \
                if (indicator || child_border) {                                                             \
                    ELOG("indicator and child_border colors have no effect for client.focused_tab_title\n"); \
                }                                                                                            \
            }                                                                                                \
            config.client.classname.border = draw_util_hex_to_color(border);                                 \
            config.client.classname.background = draw_util_hex_to_color(background);                         \
            config.client.classname.text = draw_util_hex_to_color(text);                                     \
            if (indicator != NULL) {                                                                         \
                config.client.classname.indicator = draw_util_hex_to_color(indicator);                       \
            }                                                                                                \
            if (child_border != NULL) {                                                                      \
                config.client.classname.child_border = draw_util_hex_to_color(child_border);                 \
            } else {                                                                                         \
                config.client.classname.child_border = config.client.classname.background;                   \
            }                                                                                                \
            return;                                                                                          \
        }                                                                                                    \
    } while (0)

    APPLY_COLORS(focused_inactive);
    APPLY_COLORS(focused_tab_title);
    APPLY_COLORS(focused);
    APPLY_COLORS(unfocused);
    APPLY_COLORS(urgent);
    APPLY_COLORS(placeholder);

#undef APPLY_COLORS
}

CFGFUN(assign_output, const char *output) {
    if (current_match->error != NULL) {
        ELOG("match has error: %s\n", current_match->error);
        return;
    }
    if (match_is_empty(current_match)) {
        ELOG("Match is empty, ignoring this assignment\n");
        return;
    }

    if (current_match->window_mode != WM_ANY) {
        ELOG("Assignments using window mode (floating/tiling) is not supported\n");
        return;
    }

    DLOG("New assignment, using above criteria, to output \"%s\".\n", output);
    Assignment *assignment = scalloc(1, sizeof(Assignment));
    match_copy(&(assignment->match), current_match);
    assignment->type = A_TO_OUTPUT;
    assignment->dest.output = sstrdup(output);
    TAILQ_INSERT_TAIL(&assignments, assignment, assignments);
}

CFGFUN(assign, const char *workspace, const bool is_number) {
    if (current_match->error != NULL) {
        ELOG("match has error: %s\n", current_match->error);
        return;
    }
    if (match_is_empty(current_match)) {
        ELOG("Match is empty, ignoring this assignment\n");
        return;
    }

    if (current_match->window_mode != WM_ANY) {
        ELOG("Assignments using window mode (floating/tiling) is not supported\n");
        return;
    }

    if (is_number && ws_name_to_number(workspace) == -1) {
        ELOG("Could not parse initial part of \"%s\" as a number.\n", workspace);
        return;
    }

    DLOG("New assignment, using above criteria, to workspace \"%s\".\n", workspace);
    Assignment *assignment = scalloc(1, sizeof(Assignment));
    match_copy(&(assignment->match), current_match);
    assignment->type = is_number ? A_TO_WORKSPACE_NUMBER : A_TO_WORKSPACE;
    assignment->dest.workspace = sstrdup(workspace);
    TAILQ_INSERT_TAIL(&assignments, assignment, assignments);
}

CFGFUN(no_focus) {
    if (current_match->error != NULL) {
        ELOG("match has error: %s\n", current_match->error);
        return;
    }
    if (match_is_empty(current_match)) {
        ELOG("Match is empty, ignoring this assignment\n");
        return;
    }

    DLOG("New assignment, using above criteria, to ignore focus on manage.\n");
    Assignment *assignment = scalloc(1, sizeof(Assignment));
    match_copy(&(assignment->match), current_match);
    assignment->type = A_NO_FOCUS;
    TAILQ_INSERT_TAIL(&assignments, assignment, assignments);
}

CFGFUN(ipc_kill_timeout, const long timeout_ms) {
    ipc_set_kill_timeout(timeout_ms / 1000.0);
}

CFGFUN(tiling_drag, const char *value) {
    if (strcmp(value, "modifier") == 0) {
        config.tiling_drag = TILING_DRAG_MODIFIER;
    } else if (strcmp(value, "titlebar") == 0) {
        config.tiling_drag = TILING_DRAG_TITLEBAR;
    } else if (strcmp(value, "modifier,titlebar") == 0 ||
               strcmp(value, "titlebar,modifier") == 0) {
        /* Switch the above to strtok() or similar if we ever grow more options */
        config.tiling_drag = TILING_DRAG_MODIFIER_OR_TITLEBAR;
    } else {
        config.tiling_drag = TILING_DRAG_OFF;
    }
}

/*******************************************************************************
 * Bar configuration (i3bar)
 ******************************************************************************/

static Barconfig *current_bar;

CFGFUN(bar_font, const char *font) {
    FREE(current_bar->font);
    current_bar->font = sstrdup(font);
}

CFGFUN(bar_separator_symbol, const char *separator) {
    FREE(current_bar->separator_symbol);
    current_bar->separator_symbol = sstrdup(separator);
}

CFGFUN(bar_mode, const char *mode) {
    current_bar->mode = (strcmp(mode, "dock") == 0 ? M_DOCK : (strcmp(mode, "hide") == 0 ? M_HIDE : M_INVISIBLE));
}

CFGFUN(bar_hidden_state, const char *hidden_state) {
    current_bar->hidden_state = (strcmp(hidden_state, "hide") == 0 ? S_HIDE : S_SHOW);
}

CFGFUN(bar_id, const char *bar_id) {
    current_bar->id = sstrdup(bar_id);
}

CFGFUN(bar_output, const char *output) {
    const int new_outputs = current_bar->num_outputs + 1;
    current_bar->outputs = srealloc(current_bar->outputs, sizeof(char *) * new_outputs);
    current_bar->outputs[current_bar->num_outputs] = sstrdup(output);
    current_bar->num_outputs = new_outputs;
}

CFGFUN(bar_verbose, const char *verbose) {
    current_bar->verbose = boolstr(verbose);
}

CFGFUN(bar_height, const long height) {
    current_bar->bar_height = (uint32_t)height;
}

static void dlog_padding(void) {
    DLOG("padding now: x=%d, y=%d, w=%d, h=%d\n",
         current_bar->padding.x,
         current_bar->padding.y,
         current_bar->padding.width,
         current_bar->padding.height);
}

CFGFUN(bar_padding_one, const long all) {
    current_bar->padding.x = (uint32_t)all;
    current_bar->padding.y = (uint32_t)all;
    current_bar->padding.width = (uint32_t)all;
    current_bar->padding.height = (uint32_t)all;
    dlog_padding();
}

CFGFUN(bar_padding_two, const long top_and_bottom, const long right_and_left) {
    current_bar->padding.x = (uint32_t)right_and_left;
    current_bar->padding.y = (uint32_t)top_and_bottom;
    current_bar->padding.width = (uint32_t)right_and_left;
    current_bar->padding.height = (uint32_t)top_and_bottom;
    dlog_padding();
}

CFGFUN(bar_padding_three, const long top, const long right_and_left, const long bottom) {
    current_bar->padding.x = (uint32_t)right_and_left;
    current_bar->padding.y = (uint32_t)top;
    current_bar->padding.width = (uint32_t)right_and_left;
    current_bar->padding.height = (uint32_t)bottom;
    dlog_padding();
}

CFGFUN(bar_padding_four, const long top, const long right, const long bottom, const long left) {
    current_bar->padding.x = (uint32_t)left;
    current_bar->padding.y = (uint32_t)top;
    current_bar->padding.width = (uint32_t)right;
    current_bar->padding.height = (uint32_t)bottom;
    dlog_padding();
}

CFGFUN(bar_modifier, const char *modifiers) {
    current_bar->modifier = modifiers ? event_state_from_str(modifiers) : XCB_NONE;
}

static void bar_configure_binding(const char *button, const char *release, const char *command) {
    if (strncasecmp(button, "button", strlen("button")) != 0) {
        ELOG("Bindings for a bar can only be mouse bindings, not \"%s\", ignoring.\n", button);
        return;
    }

    const int input_code = atoi(button + strlen("button"));
    if (input_code < 1) {
        ELOG("Button \"%s\" does not seem to be in format 'buttonX'.\n", button);
        return;
    }
    const bool release_bool = release != NULL;

    struct Barbinding *current;
    TAILQ_FOREACH (current, &(current_bar->bar_bindings), bindings) {
        if (current->input_code == input_code && current->release == release_bool) {
            ELOG("command for button %s was already specified, ignoring.\n", button);
            return;
        }
    }

    struct Barbinding *new_binding = scalloc(1, sizeof(struct Barbinding));
    new_binding->release = release_bool;
    new_binding->input_code = input_code;
    new_binding->command = sstrdup(command);
    TAILQ_INSERT_TAIL(&(current_bar->bar_bindings), new_binding, bindings);
}

CFGFUN(bar_wheel_up_cmd, const char *command) {
    ELOG("'wheel_up_cmd' is deprecated. Please use 'bindsym button4 %s' instead.\n", command);
    bar_configure_binding("button4", NULL, command);
}

CFGFUN(bar_wheel_down_cmd, const char *command) {
    ELOG("'wheel_down_cmd' is deprecated. Please use 'bindsym button5 %s' instead.\n", command);
    bar_configure_binding("button5", NULL, command);
}

CFGFUN(bar_bindsym, const char *button, const char *release, const char *command) {
    bar_configure_binding(button, release, command);
}

CFGFUN(bar_position, const char *position) {
    current_bar->position = (strcmp(position, "top") == 0 ? P_TOP : P_BOTTOM);
}

CFGFUN(bar_i3bar_command, const char *i3bar_command) {
    FREE(current_bar->i3bar_command);
    current_bar->i3bar_command = sstrdup(i3bar_command);
}

CFGFUN(bar_color, const char *colorclass, const char *border, const char *background, const char *text) {
#define APPLY_COLORS(classname)                                           \
    do {                                                                  \
        if (strcmp(colorclass, #classname) == 0) {                        \
            if (text != NULL) {                                           \
                /* New syntax: border, background, text */                \
                current_bar->colors.classname##_border = sstrdup(border); \
                current_bar->colors.classname##_bg = sstrdup(background); \
                current_bar->colors.classname##_text = sstrdup(text);     \
            } else {                                                      \
                /* Old syntax: text, background */                        \
                current_bar->colors.classname##_bg = sstrdup(background); \
                current_bar->colors.classname##_text = sstrdup(border);   \
            }                                                             \
        }                                                                 \
    } while (0)

    APPLY_COLORS(focused_workspace);
    APPLY_COLORS(active_workspace);
    APPLY_COLORS(inactive_workspace);
    APPLY_COLORS(urgent_workspace);
    APPLY_COLORS(binding_mode);

#undef APPLY_COLORS
}

CFGFUN(bar_socket_path, const char *socket_path) {
    FREE(current_bar->socket_path);
    current_bar->socket_path = sstrdup(socket_path);
}

CFGFUN(bar_tray_output, const char *output) {
    struct tray_output_t *tray_output = scalloc(1, sizeof(struct tray_output_t));
    tray_output->output = sstrdup(output);
    TAILQ_INSERT_TAIL(&(current_bar->tray_outputs), tray_output, tray_outputs);
}

CFGFUN(bar_tray_padding, const long padding_px) {
    current_bar->tray_padding = padding_px;
}

CFGFUN(bar_color_single, const char *colorclass, const char *color) {
    if (strcmp(colorclass, "background") == 0) {
        current_bar->colors.background = sstrdup(color);
    } else if (strcmp(colorclass, "separator") == 0) {
        current_bar->colors.separator = sstrdup(color);
    } else if (strcmp(colorclass, "statusline") == 0) {
        current_bar->colors.statusline = sstrdup(color);
    } else if (strcmp(colorclass, "focused_background") == 0) {
        current_bar->colors.focused_background = sstrdup(color);
    } else if (strcmp(colorclass, "focused_separator") == 0) {
        current_bar->colors.focused_separator = sstrdup(color);
    } else {
        current_bar->colors.focused_statusline = sstrdup(color);
    }
}

CFGFUN(bar_status_command, const char *command) {
    FREE(current_bar->status_command);
    current_bar->status_command = sstrdup(command);
}

CFGFUN(bar_workspace_command, const char *command) {
    FREE(current_bar->workspace_command);
    current_bar->workspace_command = sstrdup(command);
}

CFGFUN(bar_binding_mode_indicator, const char *value) {
    current_bar->hide_binding_mode_indicator = !boolstr(value);
}

CFGFUN(bar_workspace_buttons, const char *value) {
    current_bar->hide_workspace_buttons = !boolstr(value);
}

CFGFUN(bar_workspace_min_width, const long width) {
    current_bar->workspace_min_width = width;
}

CFGFUN(bar_strip_workspace_numbers, const char *value) {
    current_bar->strip_workspace_numbers = boolstr(value);
}

CFGFUN(bar_strip_workspace_name, const char *value) {
    current_bar->strip_workspace_name = boolstr(value);
}

CFGFUN(bar_start) {
    current_bar = scalloc(1, sizeof(Barconfig));
    TAILQ_INIT(&(current_bar->bar_bindings));
    TAILQ_INIT(&(current_bar->tray_outputs));
    current_bar->tray_padding = 2;
    current_bar->modifier = XCB_KEY_BUT_MASK_MOD_4;
}

CFGFUN(bar_finish) {
    DLOG("\t new bar configuration finished, saving.\n");
    /* Generate a unique ID for this bar if not already configured */
    if (current_bar->id == NULL) {
        sasprintf(&current_bar->id, "bar-%d", config.number_barconfigs);
    }

    config.number_barconfigs++;

    TAILQ_INSERT_TAIL(&barconfigs, current_bar, configs);
    /* Simply reset the pointer, but don't free the resources. */
    current_bar = NULL;
}
/*
 * Clamps an opacity value (0.0 - 1.0) read from the configuration.
 *
 */
static double parse_opacity(const char *value) {
    double opacity = strtod(value, NULL);
    if (opacity < 0.0) {
        opacity = 0.0;
    } else if (opacity > 1.0) {
        opacity = 1.0;
    }
    return opacity;
}

CFGFUN(animations, const char *value) {
    config.animations = boolstr(value);
    DLOG("animations = %d\n", config.animations);
}

/*
 * Trims leading and trailing whitespace of a token in place.
 *
 */
static char *cfg_trim(char *str) {
    if (str == NULL) {
        return NULL;
    }

    while (*str == ' ' || *str == '\t') {
        str++;
    }

    char *end = str + strlen(str);
    while (end > str && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';

    return str;
}

CFGFUN(bezier, const char *spec) {
    char *copy = sstrdup(spec);
    char *saveptr = NULL;
    char *name = cfg_trim(strtok_r(copy, ",", &saveptr));
    char *x1 = cfg_trim(strtok_r(NULL, ",", &saveptr));
    char *y1 = cfg_trim(strtok_r(NULL, ",", &saveptr));
    char *x2 = cfg_trim(strtok_r(NULL, ",", &saveptr));
    char *y2 = cfg_trim(strtok_r(NULL, ",", &saveptr));

    if (name == NULL || y2 == NULL) {
        ELOG("Invalid bezier curve definition \"%s\", expected "
             "\"bezier <name>, <x1>, <y1>, <x2>, <y2>\"\n",
             spec);
        FREE(copy);
        return;
    }

    animation_register_bezier(name, strtod(x1, NULL), strtod(y1, NULL),
                              strtod(x2, NULL), strtod(y2, NULL));
    DLOG("registered bezier curve \"%s\"\n", name);
    FREE(copy);
}

CFGFUN(animation, const char *spec) {
    char *copy = sstrdup(spec);
    char *saveptr = NULL;
    char *kind = cfg_trim(strtok_r(copy, ",", &saveptr));
    char *enabled = cfg_trim(strtok_r(NULL, ",", &saveptr));
    char *duration = cfg_trim(strtok_r(NULL, ",", &saveptr));
    char *curve = cfg_trim(strtok_r(NULL, ",", &saveptr));

    if (kind == NULL || *kind == '\0') {
        ELOG("Invalid animation definition \"%s\", expected "
             "\"animation <type>, <enabled>, <duration>, <curve>\"\n",
             spec);
        FREE(copy);
        return;
    }

    const bool on = (enabled != NULL && *enabled != '\0') ? boolstr(enabled) : true;
    const int ms = (duration != NULL && *duration != '\0') ? atoi(duration) : 0;
    const bool have_curve = (curve != NULL && *curve != '\0');

    const bool global = (strcasecmp(kind, "global") == 0);
    bool matched = false;

    if (global || strcasecmp(kind, "windows") == 0 ||
        strcasecmp(kind, "windowsmove") == 0) {
        config.animation_duration = on ? ms : 0;
        if (have_curve) {
            snprintf(config.animation_curve, sizeof(config.animation_curve), "%s", curve);
        }
        matched = true;
    }

    if (global || strcasecmp(kind, "windowsin") == 0 ||
        strcasecmp(kind, "windowopen") == 0) {
        config.animation_open_duration = on ? ms : 0;
        if (have_curve) {
            snprintf(config.animation_open_curve, sizeof(config.animation_open_curve),
                     "%s", curve);
        }
        matched = true;
    }

    if (global || strcasecmp(kind, "windowsout") == 0 ||
        strcasecmp(kind, "windowclose") == 0) {
        config.animation_close_duration = on ? ms : 0;
        if (have_curve) {
            snprintf(config.animation_close_curve, sizeof(config.animation_close_curve),
                     "%s", curve);
        }
        matched = true;
    }

    if (global || strcasecmp(kind, "fade") == 0 || strcasecmp(kind, "fadein") == 0 ||
        strcasecmp(kind, "fadeout") == 0) {
        config.animation_fade = on;
        matched = true;
    }

    if (global || strcasecmp(kind, "workspaces") == 0) {
        config.animation_workspace_duration = on ? ms : 0;
        if (have_curve) {
            snprintf(config.animation_workspace_curve,
                     sizeof(config.animation_workspace_curve), "%s", curve);
        }
        matched = true;
    }

    if (!matched) {
        ELOG("Unknown animation type \"%s\", expected one of windows, "
             "windowsIn, windowsOut, fadeIn, fadeOut, workspaces, global\n",
             kind);
    }

    FREE(copy);
}

CFGFUN(master_ratio, const char *value) {
    double ratio = strtod(value, NULL);
    if (ratio < 0.1 || ratio > 0.9) {
        ELOG("master_ratio must be between 0.1 and 0.9, ignoring \"%s\"\n", value);
        return;
    }
    config.master_ratio = ratio;
    DLOG("master_ratio = %f\n", config.master_ratio);
}

CFGFUN(active_opacity, const char *value) {
    config.active_opacity = parse_opacity(value);
}

CFGFUN(inactive_opacity, const char *value) {
    config.inactive_opacity = parse_opacity(value);
}

CFGFUN(dim_inactive, const char *value) {
    config.dim_inactive = boolstr(value);
}

CFGFUN(dim_strength, const char *value) {
    config.dim_strength = parse_opacity(value);
}

/*
 * Sets the corner rounding radius in pixels.
 *
 */
CFGFUN(rounding, const char *value) {
    const long radius = strtol(value, NULL, 10);
    if (radius < 0 || radius > 100) {
        ELOG("rounding must be between 0 and 100, ignoring \"%s\"\n", value);
        return;
    }

    config.rounding = logical_px(radius);
    DLOG("rounding = %d\n", config.rounding);
}

/*
 * Hyprland-style alias for the width of the border ("default_border pixel N"
 * only changes the width, the border style is controlled by "titlebar").
 *
 */
CFGFUN(border_size, const char *value) {
    const long width = strtol(value, NULL, 10);
    if (width < 0 || width > 100) {
        ELOG("border_size must be between 0 and 100, ignoring \"%s\"\n", value);
        return;
    }

    config.default_border_width = logical_px(width);
    config.default_floating_border_width = logical_px(width);
}

/*
 * Hyprland-style aliases for "gaps inner <px>" / "gaps outer <px>".
 *
 */
CFGFUN(gaps_in, const char *value) {
    apply_gaps(&config.gaps, GAPS_INNER, logical_px(strtol(value, NULL, 10)));
}

CFGFUN(gaps_out, const char *value) {
    apply_gaps(&config.gaps, GAPS_OUTER, logical_px(strtol(value, NULL, 10)));
}

/*
 * Title bar geometry, Hyprland-style.
 *
 */
CFGFUN(titlebar_height, const char *value) {
    const long height = strtol(value, NULL, 10);
    if (height < 0 || height > 200) {
        ELOG("titlebar_height must be between 0 and 200, ignoring \"%s\"\n", value);
        return;
    }

    config.titlebar_height = height > 0 ? logical_px(height) : 0;
}

CFGFUN(titlebar_padding, const char *value) {
    const long padding = strtol(value, NULL, 10);
    if (padding < 0 || padding > 100) {
        ELOG("titlebar_padding must be between 0 and 100, ignoring \"%s\"\n", value);
        return;
    }

    config.titlebar_padding = logical_px(padding);
}

/*
 * "titlebar normal|none" – hides the title bar entirely when set to none,
 * which gives the border-only look of a Hyprland window.
 *
 */
CFGFUN(titlebar_visibility, const char *value) {
    if (strcasecmp(value, "none") == 0 || strcasecmp(value, "off") == 0 ||
        strcasecmp(value, "hidden") == 0) {
        config.titlebar_hidden = true;
        /* New windows get the border-only look right away. */
        config.default_border = BS_PIXEL;
        config.default_floating_border = BS_PIXEL;
    } else if (strcasecmp(value, "normal") == 0 || strcasecmp(value, "on") == 0) {
        config.titlebar_hidden = false;
        config.default_border = BS_NORMAL;
        config.default_floating_border = BS_NORMAL;
    } else {
        ELOG("Invalid titlebar mode \"%s\", expected normal or none\n", value);
    }
}

/*
 * Parses a gradient border definition: "#rrggbb #rrggbb [angle]".
 *
 */
static void parse_gradient(struct gradient_spec *spec, const char *spec_str,
                           const char *classname) {
    char *copy = sstrdup(spec_str);
    char *saveptr = NULL;
    char *start = strtok_r(copy, " \t,", &saveptr);
    char *end = strtok_r(NULL, " \t,", &saveptr);
    char *angle_str = strtok_r(NULL, " \t,", &saveptr);

    if (start == NULL || end == NULL || start[0] != '#' || end[0] != '#') {
        ELOG("Invalid gradient for %s, expected two colors like "
             "\"#33ccff #00ff99 [45]\"\n",
             classname);
        free(copy);
        return;
    }

    const long angle = (angle_str != NULL) ? strtol(angle_str, NULL, 10) : 45;

    spec->start = draw_util_hex_to_color(start);
    spec->end = draw_util_hex_to_color(end);
    spec->angle = (int)((angle % 360 + 360) % 360);
    spec->enabled = true;

    DLOG("gradient for %s: %s -> %s at %d degrees\n",
         classname, start, end, spec->angle);

    free(copy);
}

CFGFUN(client_gradient_focused, const char *spec) {
    parse_gradient(&config.client_gradients.focused, spec, "client.focused");
}

CFGFUN(client_gradient_focused_inactive, const char *spec) {
    parse_gradient(&config.client_gradients.focused_inactive, spec,
                   "client.focused_inactive");
}

CFGFUN(client_gradient_unfocused, const char *spec) {
    parse_gradient(&config.client_gradients.unfocused, spec, "client.unfocused");
}

CFGFUN(client_gradient_urgent, const char *spec) {
    parse_gradient(&config.client_gradients.urgent, spec, "client.urgent");
}

/*
 * Fraction of the tile used by the first part of a dwindle split.
 *
 */
CFGFUN(dwindle_split_ratio, const char *value) {
    const double ratio = strtod(value, NULL);
    if (ratio < 0.1 || ratio > 0.9) {
        ELOG("dwindle_split_ratio must be between 0.1 and 0.9, ignoring \"%s\"\n", value);
        return;
    }

    config.dwindle_split_ratio = ratio;
}

/*
 * Scroller layout configuration (ported from mangowc).
 *
 */
CFGFUN(scroller_proportion, const char *value) {
    const double proportion = strtod(value, NULL);
    if (proportion < 0.1 || proportion > 1.0) {
        ELOG("scroller_proportion must be between 0.1 and 1.0, ignoring \"%s\"\n", value);
        return;
    }

    config.scroller_proportion = proportion;
}

CFGFUN(scroller_structs, const char *value) {
    const long pixels = strtol(value, NULL, 10);
    if (pixels < 0 || pixels > 1000) {
        ELOG("scroller_structs must be between 0 and 1000, ignoring \"%s\"\n", value);
        return;
    }

    config.scroller_structs = logical_px(pixels);
}

CFGFUN(scroller_focus_center, const char *value) {
    config.scroller_focus_center = boolstr(value);
}

CFGFUN(scroller_prefer_center, const char *value) {
    config.scroller_prefer_center = boolstr(value);
}

CFGFUN(scroller_prefer_overspread, const char *value) {
    config.scroller_prefer_overspread = boolstr(value);
}

CFGFUN(scroller_proportion_single, const char *value) {
    /* mangowc's scroller_ignore_proportion_single is the inverse of what we
     * store, so that "no" keeps the upstream i3 behaviour of the single
     * window filling the screen. */
    config.scroller_proportion_single = !boolstr(value);
}

CFGFUN(scroller_default_proportion_single, const char *value) {
    const double proportion = strtod(value, NULL);
    if (proportion < 0.1 || proportion > 1.0) {
        ELOG("scroller_default_proportion_single must be between 0.1 and 1.0, "
             "ignoring \"%s\"\n",
             value);
        return;
    }

    config.scroller_default_proportion_single = proportion;
}

CFGFUN(scroller_autofit, const char *value) {
    config.scroller_autofit = boolstr(value);
}
