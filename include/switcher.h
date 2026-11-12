/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * switcher.h: niri-style alt-tab window switcher with thumbnail previews
 *             (ported from mangowc's switcher).
 *
 */
#pragma once

#include <cairo.h>

#include "data.h"

/*
 * Layout constants (mirroring mangowc's switcher.h).
 *
 */
#define SW_PAD 6   /* padding between tile content and tile border */
#define SW_GAP 14  /* spacing between tiles (and rows) */
#define SW_MARGIN 28 /* outer margin of the panel */

#define SW_MAX_CANDIDATES 64
#define SW_TITLE_GAP 4

/*
 * Opens the switcher (or, if it is already open, cycles the selection) in the
 * given direction. This is the `switcher next|prev` command.
 *
 */
void switcher_next(int direction);

/*
 * Focuses the currently highlighted window and closes the switcher.
 *
 */
void switcher_commit(void);

/*
 * Closes the switcher without changing the focus.
 *
 */
void switcher_close(void);

/*
 * Whether the switcher is currently open.
 *
 */
bool switcher_is_active(void);

/*
 * Handles a KeyPress/KeyRelease while the switcher is open. Returns true if
 * the event was consumed (and must not be processed as a binding).
 *
 */
bool switcher_handle_key(xcb_key_press_event_t *event);

/*
 * Handles a button press on the switcher panel. Returns true if the event was
 * consumed.
 *
 */
bool switcher_handle_button(xcb_button_press_event_t *event);

/*
 * Redraws the panel after an expose event on it.
 *
 */
void switcher_handle_expose(xcb_window_t window);
