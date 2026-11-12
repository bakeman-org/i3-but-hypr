/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * dropdown.h: Yakuake/niri-style dropdown windows. A dropdown window is kept
 *             alive in the scratchpad while hidden and shown again as a panel
 *             attached to the top or bottom edge of the workspace.
 *
 */
#pragma once

#include "data.h"

#define DROPDOWN_POSITION_TOP 0
#define DROPDOWN_POSITION_BOTTOM 1

#define DROPDOWN_ALIGN_LEFT 0
#define DROPDOWN_ALIGN_CENTER 1
#define DROPDOWN_ALIGN_RIGHT 2

/**
 * Whether the given container (or the focused client inside it) is a dropdown
 * window.
 *
 */
bool con_is_dropdown(Con *con);

/**
 * Shows the given window as a dropdown on the currently focused workspace.
 * A non-NULL spec applies per-invocation geometry to the client first.
 *
 */
bool dropdown_show(Con *con, const char *spec);

/**
 * Hides the given dropdown window into the scratchpad.
 *
 */
bool dropdown_hide(Con *con);

/**
 * Toggles the given window between the visible dropdown and the scratchpad.
 *
 */
bool dropdown_toggle(Con *con, const char *spec);

/**
 * `dropdown toggle` without criteria: hides the focused dropdown or turns the
 * focused window into a dropdown and shows it.
 *
 */
bool dropdown_toggle_global(const char *spec);

/**
 * `dropdown show` without criteria.
 *
 */
bool dropdown_show_global(const char *spec);

/**
 * Re-opens the most recently hidden dropdown on the focused workspace.
 *
 */
bool dropdown_recall(const char *spec);

/**
 * `dropdown hide` without criteria.
 *
 */
bool dropdown_hide_global(void);
