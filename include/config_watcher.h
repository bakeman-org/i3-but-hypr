/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * config_watcher.h: inotify-based automatic reload of the configuration
 * file (Hyprland-style hot reload).
 *
 */
#pragma once

/*
 * Sets up the inotify watch on the configuration files. Must be called
 * after the initial configuration has been loaded and main_loop exists.
 *
 */
void config_watcher_init(void);

/*
 * Rebuilds the watch list from the currently loaded configuration. Call
 * after every reload, since the set of included files may have changed.
 *
 */
void config_watcher_rearm(void);

/*
 * Kills the error nagbar of the hot reload watcher (if any) and forgets its
 * pid. The watcher owns a separate pid which the generic config/command nagbar
 * cleanup does not know about, so this must be called when i3 restarts or
 * exits, and when a manual reload succeeds.
 *
 */
void config_watcher_kill_nagbar(void);
