/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * status_line.h: an always-visible state line (mode, layout, focus, last action).
 */
#pragma once

#include <config.h>

/**
 * Recomputes the text and redraws the status line if it changed. Safe to call
 * on every render; it does nothing when "status_line" is disabled.
 *
 */
void status_line_update(void);

/**
 * Remembers the command that is about to be executed, so that the status line
 * can show it (like a verbose log of the last action).
 *
 */
void status_line_set_last_command(const char *command);

/**
 * Destroys the status line window.
 *
 */
void status_line_hide(void);
