/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * config.c: Configuration file (calling the parser (src/config_parser.c) with
 *           the correct path, switching key bindings mode).
 *
 */
#include "all.h"

#include <libgen.h>
#include <xkbcommon/xkbcommon.h>

char *current_configpath = NULL;
Config config;
struct modes_head modes;
struct barconfig_head barconfigs = TAILQ_HEAD_INITIALIZER(barconfigs);
struct includedfiles_head included_files = TAILQ_HEAD_INITIALIZER(included_files);

/*
 * Ungrabs all keys, to be called before re-grabbing the keys because of a
 * mapping_notify event or a configuration file reload
 *
 */
void ungrab_all_keys(xcb_connection_t *conn) {
    DLOG("Ungrabbing all keys\n");
    xcb_ungrab_key(conn, XCB_GRAB_ANY, root, XCB_BUTTON_MASK_ANY);
}

static void free_configuration(void) {
    assert(conn != NULL);

    /* If we are currently in a binding mode, we first revert to the default
     * since we have no guarantee that the current mode will even still exist
     * after parsing the config again. See #2228. */
    switch_mode("default");

    /* First ungrab the keys */
    ungrab_all_keys(conn);

    while (!SLIST_EMPTY(&modes)) {
        struct Mode *mode = SLIST_FIRST(&modes);
        FREE(mode->name);

        /* Clear the old binding list */
        while (!TAILQ_EMPTY(mode->bindings)) {
            Binding *bind = TAILQ_FIRST(mode->bindings);
            TAILQ_REMOVE(mode->bindings, bind, bindings);
            binding_free(bind);
        }
        FREE(mode->bindings);

        SLIST_REMOVE(&modes, mode, Mode, modes);
        FREE(mode);
    }

    while (!TAILQ_EMPTY(&assignments)) {
        Assignment *assign = TAILQ_FIRST(&assignments);
        if (assign->type == A_TO_WORKSPACE || assign->type == A_TO_WORKSPACE_NUMBER) {
            FREE(assign->dest.workspace);
        } else if (assign->type == A_COMMAND) {
            FREE(assign->dest.command);
        } else if (assign->type == A_TO_OUTPUT) {
            FREE(assign->dest.output);
        }
        match_free(&(assign->match));
        TAILQ_REMOVE(&assignments, assign, assignments);
        FREE(assign);
    }

    while (!TAILQ_EMPTY(&ws_assignments)) {
        struct Workspace_Assignment *assign = TAILQ_FIRST(&ws_assignments);
        FREE(assign->name);
        FREE(assign->output);
        TAILQ_REMOVE(&ws_assignments, assign, ws_assignments);
        FREE(assign);
    }

    /* Clear bar configs */
    while (!TAILQ_EMPTY(&barconfigs)) {
        Barconfig *barconfig = TAILQ_FIRST(&barconfigs);
        FREE(barconfig->id);
        for (int c = 0; c < barconfig->num_outputs; c++) {
            free(barconfig->outputs[c]);
        }

        while (!TAILQ_EMPTY(&(barconfig->bar_bindings))) {
            struct Barbinding *binding = TAILQ_FIRST(&(barconfig->bar_bindings));
            FREE(binding->command);
            TAILQ_REMOVE(&(barconfig->bar_bindings), binding, bindings);
            FREE(binding);
        }

        while (!TAILQ_EMPTY(&(barconfig->tray_outputs))) {
            struct tray_output_t *tray_output = TAILQ_FIRST(&(barconfig->tray_outputs));
            FREE(tray_output->output);
            TAILQ_REMOVE(&(barconfig->tray_outputs), tray_output, tray_outputs);
            FREE(tray_output);
        }

        FREE(barconfig->outputs);
        FREE(barconfig->socket_path);
        FREE(barconfig->status_command);
        FREE(barconfig->workspace_command);
        FREE(barconfig->i3bar_command);
        FREE(barconfig->font);
        FREE(barconfig->colors.background);
        FREE(barconfig->colors.statusline);
        FREE(barconfig->colors.separator);
        FREE(barconfig->colors.focused_background);
        FREE(barconfig->colors.focused_statusline);
        FREE(barconfig->colors.focused_separator);
        FREE(barconfig->colors.focused_workspace_border);
        FREE(barconfig->colors.focused_workspace_bg);
        FREE(barconfig->colors.focused_workspace_text);
        FREE(barconfig->colors.active_workspace_border);
        FREE(barconfig->colors.active_workspace_bg);
        FREE(barconfig->colors.active_workspace_text);
        FREE(barconfig->colors.inactive_workspace_border);
        FREE(barconfig->colors.inactive_workspace_bg);
        FREE(barconfig->colors.inactive_workspace_text);
        FREE(barconfig->colors.urgent_workspace_border);
        FREE(barconfig->colors.urgent_workspace_bg);
        FREE(barconfig->colors.urgent_workspace_text);
        FREE(barconfig->colors.binding_mode_border);
        FREE(barconfig->colors.binding_mode_bg);
        FREE(barconfig->colors.binding_mode_text);
        TAILQ_REMOVE(&barconfigs, barconfig, configs);
        FREE(barconfig);
    }

    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        /* Assignments changed, previously ran assignments are invalid. */
        if (con->window) {
            con->window->nr_assignments = 0;
            FREE(con->window->ran_assignments);
        }
        /* Invalidate pixmap caches in case font or colors changed. */
        FREE(con->deco_render_params);
    }

    /* Get rid of the current font */
    free_font();

    /* An explicitly configured titlebar font owns its resources
     * independently of the global font; a reload discards the whole
     * configuration, so release them here as well. */
    free_font_struct(&config.titlebar_font);

    free(config.ipc_socket_path);
    free(config.restart_state_path);
    free(config.fake_outputs);
}

/*
 * (Re-)loads the configuration file (sets useful defaults before).
 *
 * If you specify override_configpath, only this path is used to look for a
 * configuration file.
 *
 * load_type specifies the type of loading: C_VALIDATE is used to only verify
 * the correctness of the config file (used with the flag -C). C_LOAD will load
 * the config for normal use and display errors in the nagbar. C_RELOAD will
 * also clear the previous config.
 *
 */
bool load_configuration(const char *override_configpath, config_load_t load_type) {
    if (load_type == C_RELOAD) {
        free_configuration();
    }

    SLIST_INIT(&modes);

    struct Mode *default_mode = scalloc(1, sizeof(struct Mode));
    default_mode->name = sstrdup("default");
    default_mode->bindings = scalloc(1, sizeof(struct bindings_head));
    TAILQ_INIT(default_mode->bindings);
    SLIST_INSERT_HEAD(&modes, default_mode, modes);

    bindings = default_mode->bindings;
    current_binding_mode = default_mode->name;

    /* Clear the old config or initialize the data structure */
    memset(&config, 0, sizeof(config));

    /* Initialize default colors */
#define INIT_COLOR(x, cborder, cbackground, ctext, cindicator) \
    do {                                                       \
        x.border = draw_util_hex_to_color(cborder);            \
        x.background = draw_util_hex_to_color(cbackground);    \
        x.text = draw_util_hex_to_color(ctext);                \
        x.indicator = draw_util_hex_to_color(cindicator);      \
        x.child_border = draw_util_hex_to_color(cbackground);  \
    } while (0)

    config.client.background = draw_util_hex_to_color("#11111b");
    /* Watch configuration files and reload automatically by default. */
    config.hot_reload = true;
    /* Show the WM state line (mode, layout, focus, last action) by default. */
    config.status_line = true;
    /* Showing the last key chord is opt-in via i3ext { key_hints = true }. */
    config.key_hints = false;
    /* Tracing every key press is opt-in via i3ext { key_trace = true }. */
    config.key_trace = false;
    config.key_trace_count = 10;
    INIT_COLOR(config.client.focused, "#181825", "#181825", "#cdd6f4", "#f9e2af");
    INIT_COLOR(config.client.focused_inactive, "#181825", "#181825", "#a6adc8", "#313244");
    INIT_COLOR(config.client.unfocused, "#11111b", "#11111b", "#6c7086", "#313244");
    INIT_COLOR(config.client.urgent, "#11111b", "#181825", "#f38ba8", "#f38ba8");
    config.client.got_focused_tab_title = false;

    /* border and indicator color are ignored for placeholder contents */
    INIT_COLOR(config.client.placeholder, "#000000", "#0c0c0c", "#ffffff", "#000000");

    /* the last argument (indicator color) is ignored for bar colors */
    INIT_COLOR(config.bar.focused, "#4c7899", "#285577", "#ffffff", "#000000");
    INIT_COLOR(config.bar.unfocused, "#333333", "#222222", "#888888", "#000000");
    INIT_COLOR(config.bar.urgent, "#2f343a", "#900000", "#ffffff", "#000000");

    config.show_marks = true;

    /* Hyprland-flavored out-of-box look; explicit config directives always
     * override these. Set rounding back to 0 for the plain upstream look. */
    config.rounding = 12;
    config.titlebar_height = 0;
    config.titlebar_padding = 2;
    config.titlebar_hidden = false;
    config.titlebar_icon_padding = 2;
    config.title_align = ALIGN_CENTER;
    memset(&config.client_gradients, 0, sizeof(config.client_gradients));
    config.client_gradients.focused.angle = 45;
    config.client_gradients.focused_inactive.angle = 45;
    config.client_gradients.unfocused.angle = 45;
    config.client_gradients.urgent.angle = 45;

    config.default_border = BS_NORMAL;
    config.default_floating_border = BS_NORMAL;
    config.default_border_width = logical_px(1);
    config.default_floating_border_width = logical_px(1);
    /* Set default_orientation to NO_ORIENTATION for auto orientation. */
    config.default_orientation = NO_ORIENTATION;

    config.gaps.inner = 4;
    config.gaps.top = 8;
    config.gaps.right = 8;
    config.gaps.bottom = 8;
    config.gaps.left = 8;

    /* Set default urgency reset delay to 500ms */
    if (config.workspace_urgency_timer == 0) {
        config.workspace_urgency_timer = 0.5;
    }

    config.focus_wrapping = FOCUS_WRAPPING_ON;

    /* Hyprland-inspired animation defaults. */
    config.animations = true;
    config.animation_duration = 180;
    config.animation_workspace_duration = 300;
    config.animation_open_duration = 220;
    config.animation_close_duration = 150;
    config.animation_dropdown_duration = 250;
    strcpy(config.animation_curve, "easeOutQuint");
    strcpy(config.animation_workspace_curve, "easeOutQuint");
    strcpy(config.animation_open_curve, "easeOutQuint");
    strcpy(config.animation_close_curve, "easeOutQuint");
    strcpy(config.animation_dropdown_curve, "easeOutQuint");
    config.animation_fade = true;
    config.dropdown_height = 0.5;
    config.dropdown_position = 0;
    config.master_ratio = 0.6;
    config.master_orientation = 0;
    config.dwindle_split_ratio = 0.5;
    config.scroller_proportion = 0.9;
    config.scroller_structs = 20;
    config.scroller_focus_center = false;
    config.scroller_prefer_center = false;
    config.scroller_prefer_overspread = true;
    config.scroller_proportion_single = false;
    config.scroller_default_proportion_single = 1.0;
    config.scroller_autofit = true;
    config.active_opacity = 1.0;
    config.inactive_opacity = 1.0;
    config.dim_inactive = false;
    config.dim_strength = 0.15;
    animation_reset_beziers();

    config.tiling_drag = TILING_DRAG_MODIFIER;
    config.swap_modifier = XCB_KEY_BUT_MASK_SHIFT;

    FREE(current_configpath);
    current_configpath = get_config_path(override_configpath, true);
    if (current_configpath == NULL) {
        die("Unable to find the configuration file (looked at "
            "$XDG_CONFIG_HOME/i3/config, ~/.i3/config, $XDG_CONFIG_DIRS/i3/config "
            "and " SYSCONFDIR "/i3/config)");
    }

    IncludedFile *file;
    while (!TAILQ_EMPTY(&included_files)) {
        file = TAILQ_FIRST(&included_files);
        FREE(file->path);
        FREE(file->raw_contents);
        FREE(file->variable_replaced_contents);
        TAILQ_REMOVE(&included_files, file, files);
        FREE(file);
    }

    char resolved_path[PATH_MAX] = {'\0'};
    if (realpath(current_configpath, resolved_path) == NULL) {
        die("realpath(%s): %s", current_configpath, strerror(errno));
    }

    file = scalloc(1, sizeof(IncludedFile));
    file->path = sstrdup(resolved_path);
    TAILQ_INSERT_TAIL(&included_files, file, files);

    LOG("Parsing configfile %s\n", resolved_path);
    /* Drop the Lua chunks of the previous configuration; they are
     * re-registered while the new configuration (and its includes) is
     * parsed. */
    i3ext_registry_reset();
    struct stack stack = {0};
    struct parser_ctx ctx = {
        .use_nagbar = (load_type != C_VALIDATE),
        .stack = &stack,
    };
    SLIST_INIT(&(ctx.variables));
    const int result = parse_file(&ctx, resolved_path, file);
    free_variables(&ctx);
    if (result == -1) {
        die("Could not open configuration file: %s\n", strerror(errno));
    }

    extract_workspace_names_from_bindings();
    reorder_bindings();

    if (config.font.type == FONT_TYPE_NONE && load_type != C_VALIDATE) {
        ELOG("You did not specify required configuration option \"font\"\n");
        config.font = load_font("fixed", true);
        set_font(&config.font);
    }

    /* Title bars fall back to the global font when no titlebar_font was
     * set; the use sites select the font dynamically, so no ownership is
     * shared here. */

    /* Make bar config blocks without a configured font use the i3-wide font. */
    if (load_type != C_VALIDATE) {
        Barconfig *current;
        TAILQ_FOREACH (current, &barconfigs, configs) {
            if (current->font != NULL) {
                continue;
            }
            current->font = sstrdup(config.font.pattern);
        }
    }

    if (load_type == C_RELOAD) {
        translate_keysyms();
        grab_all_keys(conn);
        regrab_all_buttons(conn);
        gaps_reapply_workspace_assignments();
        x_update_frame_backgrounds();
        animation_reapply_opacity();

        /* Redraw the currently visible decorations on reload, so that the
         * possibly new drawing parameters changed. */
        tree_render();
    }

    return result == 0;
}
