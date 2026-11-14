/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * status_line.h: publishes WM state (mode, layout, focus, last action) to i3bar.
 */
#pragma once

#include <config.h>

/**
 * Recomputes the text and publishes it to i3bar if it changed. Safe to call
 * on every render; it does nothing when "status_line" is disabled.
 *
 */
void status_line_update(void);

/**
 * Clears the cached text and publishes unconditionally. Used when a new i3bar
 * client subscribes, so that the bar immediately shows the current state.
 *
 */
void status_line_refresh(void);

/**
 * Remembers the key chord of the last matched binding, so that the status
 * line can show it (screenkey-style). The text is published by the next
 * status_line_update() call, which happens right after parsing the command.
 *
 */
void status_line_note_key(uint32_t state_mask, const char *symbol);

/**
 * Remembers the command that is about to be executed, so that the status line
 * can show it (like a verbose log of the last action).
 *
 */
void status_line_set_last_command(const char *command);

/**
 * Stops publishing the status text and restores the plain binding mode.
 *
 */
void status_line_hide(void);
