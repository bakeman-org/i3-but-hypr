/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * key_trace.h: screenkey-style tracking of every key press via XInput2 raw
 * events. Raw events are delivered in addition to the normal events, so they
 * never consume or alter what the focused application receives.
 */
#pragma once

#include <config.h>

/**
 * Selects XInput2 raw key events on all screens and creates the private xkb
 * state used to translate keycodes/modifiers. Safe to call once at startup.
 *
 */
void key_trace_init(void);

/**
 * Recreates the private xkb state after i3 reloaded the keymap (layout or
 * keycode changes).
 *
 */
void key_trace_reload_keymap(void);

/**
 * Forgets which keycodes are currently held. Called on focus changes so a
 * release that happened while another window had the keyboard cannot leave a
 * key marked as down (and suppress the next real press).
 *
 */
void key_trace_reset_keys(void);

/**
 * Handles a generic X11 event. Returns true if it was a raw key event that
 * we consumed, false otherwise.
 *
 */
bool key_trace_handle_event(xcb_generic_event_t *event);
