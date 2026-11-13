/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * mode_hint.h: an on-screen hint for binding modes (e.g. resize).
 */
#pragma once

#include <config.h>

struct Mode;

/**
 * Shows a hint overlay with the name of the given mode and its keyboard
 * bindings. Does nothing if "mode_hint" is disabled or the mode has no
 * keyboard bindings.
 *
 */
void mode_hint_show(const struct Mode *mode);

/**
 * Removes the hint overlay (if it is currently shown).
 *
 */
void mode_hint_hide(void);
