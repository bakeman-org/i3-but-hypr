/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * key_trace.c: screenkey-style tracking of every key press.
 *
 * i3 only receives key events for keys it grabbed. To display *all* keys
 * without grabbing (which would swallow input), we listen to XInput2 raw key
 * events: they are delivered in parallel to the normal event stream and do not
 * affect the focused application. A private xkb state (fed with every raw
 * press/release) provides the current modifier state and keysym.
 *
 */
#include "all.h"

#include <xcb/xinput.h>
#include <xkbcommon/xkbcommon.h>

/* XIAllMasterDevices */
#define XI_ALL_MASTER_DEVICES 1

static int xinput_opcode = -1;
static struct xkb_state *raw_state;
static xkb_mod_index_t mod_ctrl;
static xkb_mod_index_t mod_shift;
static xkb_mod_index_t mod_alt;
static xkb_mod_index_t mod_super;

/* Maps the eight core modifier bits (Shift, Lock, Control, Mod1..Mod5) to the
 * xkb modifier indices of the current keymap. */
static xkb_mod_index_t core_mod_index[8];

static xcb_window_t trace_root = XCB_NONE;

/* Bitmap of keycodes currently held down (to filter auto-repeat). */
static uint8_t keys_down[32];

static const char *const core_mod_names[8] = {
    "Shift", "Lock", "Control", "Mod1", "Mod2", "Mod3", "Mod4", "Mod5"};

static void reset_keymap_state(void) {
    if (raw_state != NULL) {
        xkb_state_unref(raw_state);
        raw_state = NULL;
    }
    memset(keys_down, 0, sizeof(keys_down));

    struct xkb_keymap *keymap = bindings_get_keymap();
    if (keymap == NULL) {
        return;
    }

    raw_state = xkb_state_new(keymap);
    if (raw_state == NULL) {
        ELOG("key_trace: could not create xkb state\n");
        return;
    }

    for (int i = 0; i < 8; i++) {
        core_mod_index[i] = xkb_keymap_mod_get_index(keymap, core_mod_names[i]);
    }

    mod_ctrl = core_mod_index[2];
    mod_shift = core_mod_index[0];
    mod_alt = core_mod_index[3];
    mod_super = core_mod_index[6];
}

/*
 * Raw key events do not carry the modifier state, and i3 may miss a release
 * when the keyboard focus leaves the nested/real session while a modifier is
 * held (e.g. Super). Ask the server for the current core modifier mask before
 * formatting each key and force our private xkb state to match. This keeps
 * plain keys from being prefixed with a stale "Super+"/"Ctrl+".
 *
 */
static void resync_modifiers(void) {
    if (raw_state == NULL || trace_root == XCB_NONE) {
        return;
    }

    xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(
        conn, xcb_query_pointer(conn, trace_root), NULL);
    if (reply == NULL) {
        return;
    }
    const uint16_t mask = reply->mask;
    free(reply);

    xkb_mod_mask_t depressed = 0;
    xkb_mod_mask_t locked = 0;
    for (int i = 0; i < 8; i++) {
        if (core_mod_index[i] == XKB_MOD_INVALID ||
            (mask & (1u << i)) == 0) {
            continue;
        }
        const xkb_mod_mask_t bit = 1u << core_mod_index[i];
        if (i == 1) {
            locked |= bit; /* CapsLock */
        } else {
            depressed |= bit;
        }
    }

    xkb_state_update_mask(raw_state, depressed, 0, locked, 0, 0,
                          xkb_state_serialize_group(raw_state, XKB_STATE_LAYOUT_EFFECTIVE));
}

void key_trace_reload_keymap(void) {
    reset_keymap_state();
}

void key_trace_reset_keys(void) {
    memset(keys_down, 0, sizeof(keys_down));
}

void key_trace_init(void) {
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(conn, &xcb_input_id);
    if (ext == NULL || !ext->present) {
        LOG("key_trace: XInput extension not available, key tracing disabled\n");
        return;
    }

    xcb_input_xi_query_version_reply_t *version = xcb_input_xi_query_version_reply(
        conn, xcb_input_xi_query_version(conn, 2, 0), NULL);
    if (version == NULL || version->major_version < 2) {
        LOG("key_trace: XInput2 not available, key tracing disabled\n");
        free(version);
        return;
    }
    free(version);

    xinput_opcode = ext->major_opcode;

    struct {
        xcb_input_event_mask_t head;
        uint32_t mask;
    } event_mask;
    event_mask.head.deviceid = XI_ALL_MASTER_DEVICES;
    event_mask.head.mask_len = 1;
    event_mask.mask = XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS |
                      XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE;

    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
    for (; iter.rem > 0; xcb_screen_next(&iter)) {
        if (trace_root == XCB_NONE) {
            trace_root = iter.data->root;
        }
        xcb_input_xi_select_events(conn, iter.data->root, 1, &event_mask.head);
    }
    xcb_flush(conn);

    reset_keymap_state();
    LOG("key_trace: raw key event tracing enabled\n");
}

static bool is_modifier_key(xkb_keysym_t sym, const char **label) {
    switch (sym) {
        case XKB_KEY_Shift_L:
        case XKB_KEY_Shift_R:
            *label = "Shift";
            return true;
        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R:
            *label = "Ctrl";
            return true;
        case XKB_KEY_Alt_L:
        case XKB_KEY_Alt_R:
        case XKB_KEY_Meta_L:
        case XKB_KEY_Meta_R:
            *label = "Alt";
            return true;
        case XKB_KEY_Super_L:
        case XKB_KEY_Super_R:
        case XKB_KEY_Hyper_L:
        case XKB_KEY_Hyper_R:
            *label = "Super";
            return true;
        default:
            return false;
    }
}

static bool mod_active(xkb_mod_index_t index) {
    return raw_state != NULL && index != XKB_MOD_INVALID &&
           xkb_state_mod_index_is_active(raw_state, index, XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void format_key(xkb_keysym_t sym, char *out, size_t len) {
    const char *modifier = NULL;
    if (is_modifier_key(sym, &modifier)) {
        snprintf(out, len, "%s", modifier);
        return;
    }

    char utf8[8] = "";
    const int utf8_len = xkb_keysym_to_utf8(sym, utf8, sizeof(utf8));
    const bool printable = utf8_len > 0 && (unsigned char)utf8[0] >= 0x20;

    const char *special = NULL;
    switch (sym) {
        case XKB_KEY_space:
            special = "Space";
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            special = "Return";
            break;
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab:
            special = "Tab";
            break;
        case XKB_KEY_BackSpace:
            special = "Bksp";
            break;
        case XKB_KEY_Escape:
            special = "Esc";
            break;
        case XKB_KEY_Delete:
            special = "Del";
            break;
        default:
            break;
    }

    char name[64];
    if (special == NULL) {
        if (printable) {
            special = utf8;
        } else {
            if (xkb_keysym_get_name(sym, name, sizeof(name)) <= 0) {
                snprintf(name, sizeof(name), "0x%x", sym);
            }
            special = name;
        }
    }

    char prefix[48] = "";
    if (mod_active(mod_ctrl)) {
        strlcat(prefix, "Ctrl+", sizeof(prefix));
    }
    if (mod_active(mod_alt)) {
        strlcat(prefix, "Alt+", sizeof(prefix));
    }
    if (mod_active(mod_super)) {
        strlcat(prefix, "Super+", sizeof(prefix));
    }
    if (!printable && mod_active(mod_shift)) {
        strlcat(prefix, "Shift+", sizeof(prefix));
    }

    snprintf(out, len, "%s%s", prefix, special);
}

static void handle_raw_key(const xcb_input_raw_key_press_event_t *event, bool down) {
    const xcb_keycode_t keycode = (xcb_keycode_t)event->detail;
    if (raw_state == NULL || keycode == 0) {
        return;
    }

    const size_t byte = keycode / 8;
    const uint8_t bit = 1 << (keycode % 8);
    const bool was_down = (keys_down[byte] & bit) != 0;

    if (down) {
        /* Auto-repeat sends repeated presses without a release in between.
         * Keep the state consistent, but do not flood the bar with repeats. */
        if (was_down) {
            return;
        }
        keys_down[byte] |= bit;
        xkb_state_update_key(raw_state, keycode, XKB_KEY_DOWN);
    } else {
        if (!was_down) {
            return;
        }
        keys_down[byte] &= ~bit;
        xkb_state_update_key(raw_state, keycode, XKB_KEY_UP);
        return;
    }

    if (!config.key_trace) {
        return;
    }

    /* Force the modifier state to match the server before formatting, so a
     * missed release (focus change, nested session) cannot leave a stale
     * "Super+"/"Ctrl+" prefix on plain keys. */
    resync_modifiers();

    const xkb_keysym_t sym = xkb_state_key_get_one_sym(raw_state, keycode);
    char label[96];
    format_key(sym, label, sizeof(label));
    status_line_append_key(label);
}

bool key_trace_handle_event(xcb_generic_event_t *event) {
    if (xinput_opcode < 0) {
        return false;
    }

    xcb_ge_generic_event_t *generic = (xcb_ge_generic_event_t *)event;
    if ((generic->response_type & ~0x80) != XCB_GE_GENERIC ||
        generic->extension != xinput_opcode) {
        return false;
    }

    if (generic->event_type != XCB_INPUT_RAW_KEY_PRESS &&
        generic->event_type != XCB_INPUT_RAW_KEY_RELEASE) {
        return false;
    }

    const xcb_input_raw_key_press_event_t *raw =
        (const xcb_input_raw_key_press_event_t *)event;
    handle_raw_key(raw, generic->event_type == XCB_INPUT_RAW_KEY_PRESS);
    return true;
}
