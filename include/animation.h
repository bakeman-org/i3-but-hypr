/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * animation.h: Hyprland-style smooth animations (bezier curves, geometry
 *              interpolation, workspace sliding and opacity fading).
 *
 */
#pragma once

#include "data.h"

/**
 * Initializes the animation subsystem (registers the frame timer).
 *
 */
void animation_init(void);

/**
 * Removes all user defined bezier curves. Called whenever the configuration is
 * (re-)parsed.
 *
 */
void animation_reset_beziers(void);

/**
 * Registers a bezier curve (Hyprland's `bezier` config directive).
 *
 */
void animation_register_bezier(const char *name, double x1, double y1, double x2, double y2);

/**
 * Converts a (unsigned) layout rectangle into a signed animation rectangle.
 *
 */
anim_rect animation_rect_from_layout(Rect rect);

/**
 * Starts an animation from the given rectangle to the given rectangle.
 *
 */
void animation_begin(Con *con, anim_rect from, anim_rect to, const char *curve, double duration, bool fade);

/**
 * Marks all containers of the given workspace to slide in from the given
 * horizontal offset on their next push.
 *
 */
void animation_slide_in(Con *workspace, int dx);

/**
 * Whether any animation is currently running.
 *
 */
bool animation_running(void);

/**
 * Sets the _NET_WM_WINDOW_OPACITY property of the given container's window.
 *
 */
void animation_set_opacity(Con *con, double opacity);

/**
 * Applies the configured opacity for the given container, taking the focus
 * state and `dim_inactive` into account.
 *
 */
void animation_apply_opacity(Con *con);

/**
 * Re-applies the opacity of the old and the new focused container.
 *
 */
void animation_focus_changed(Con *old, Con *new);
