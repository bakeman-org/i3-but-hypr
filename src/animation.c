/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * animation.c: Hyprland-style animations. Geometry changes are interpolated
 *              between the last rectangle that was pushed to X11 and the
 *              rectangle the layout engine computed, which keeps the layout
 *              code completely free of animation logic.
 *
 */
#include "all.h"

#include <math.h>

#define MAX_CUSTOM_BEZIERS 16
#define BEZIER_BUILTIN_OFFSET 1000

typedef struct {
    char name[64];
    double x1, y1, x2, y2;
} bezier_def;

static const bezier_def builtin_beziers[] = {
    {"default", 0.23, 1.0, 0.32, 1.0},
    {"linear", 0.0, 0.0, 1.0, 1.0},
    {"easeOutCubic", 0.33, 1.0, 0.68, 1.0},
    {"easeOutQuint", 0.23, 1.0, 0.32, 1.0},
    {"easeOutExpo", 0.16, 1.0, 0.30, 1.0},
    {"easeInOutCubic", 0.65, 0.0, 0.35, 1.0},
    {"easeInOutQuint", 0.86, 0.0, 0.07, 1.0},
    {"easeInOutSine", 0.37, 0.0, 0.63, 1.0},
};

static bezier_def custom_beziers[MAX_CUSTOM_BEZIERS];
static int custom_beziers_count = 0;

static ev_timer anim_timer;
static bool timer_started = false;

static bool anim_rect_equals(anim_rect a, anim_rect b);

static xcb_atom_t atom_opacity = XCB_NONE;

/******************************************************************************
 * Bezier curves
 ******************************************************************************/

void animation_reset_beziers(void) {
    custom_beziers_count = 0;
}

void animation_register_bezier(const char *name, double x1, double y1, double x2, double y2) {
    if (name == NULL || *name == '\0') {
        return;
    }

    for (int i = 0; i < custom_beziers_count; i++) {
        if (strcmp(custom_beziers[i].name, name) == 0) {
            custom_beziers[i] = (bezier_def){.x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2};
            snprintf(custom_beziers[i].name, sizeof(custom_beziers[i].name), "%s", name);
            return;
        }
    }

    if (custom_beziers_count >= MAX_CUSTOM_BEZIERS) {
        ELOG("Too many bezier curves defined, ignoring \"%s\"\n", name);
        return;
    }

    bezier_def *def = &(custom_beziers[custom_beziers_count++]);
    snprintf(def->name, sizeof(def->name), "%s", name);
    def->x1 = x1;
    def->y1 = y1;
    def->x2 = x2;
    def->y2 = y2;
}

static const bezier_def *builtin_by_index(int idx) {
    const int count = sizeof(builtin_beziers) / sizeof(builtin_beziers[0]);
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= count) {
        idx = 0;
    }
    return &(builtin_beziers[idx]);
}

static int animation_curve_id(const char *name) {
    if (name == NULL || *name == '\0') {
        name = "default";
    }

    for (int i = 0; i < custom_beziers_count; i++) {
        if (strcmp(custom_beziers[i].name, name) == 0) {
            return i;
        }
    }

    const int count = sizeof(builtin_beziers) / sizeof(builtin_beziers[0]);
    for (int i = 0; i < count; i++) {
        if (strcmp(builtin_beziers[i].name, name) == 0) {
            return BEZIER_BUILTIN_OFFSET + i;
        }
    }

    return BEZIER_BUILTIN_OFFSET;
}

static const bezier_def *animation_curve(int id) {
    if (id >= BEZIER_BUILTIN_OFFSET) {
        return builtin_by_index(id - BEZIER_BUILTIN_OFFSET);
    }
    if (id < 0 || id >= custom_beziers_count) {
        return builtin_by_index(0);
    }
    return &(custom_beziers[id]);
}

/*
 * Evaluates a cubic bezier curve with the control points (0, 0), (x1, y1),
 * (x2, y2) and (1, 1) at the given x position, i.e. it solves X(u) = x for u
 * using Newton-Raphson and returns Y(u).
 *
 */
static double bezier_eval(const bezier_def *curve, double x) {
    if (x <= 0.0) {
        return 0.0;
    }
    if (x >= 1.0) {
        return 1.0;
    }

    double u = x;
    for (int i = 0; i < 8; i++) {
        const double one_minus_u = 1.0 - u;
        const double current_x = 3 * one_minus_u * one_minus_u * u * curve->x1 +
                                 3 * one_minus_u * u * u * curve->x2 +
                                 u * u * u;
        const double derivative = 3 * one_minus_u * one_minus_u * curve->x1 +
                                  6 * one_minus_u * u * (curve->x2 - curve->x1) +
                                  3 * u * u * (1.0 - curve->x2);
        if (fabs(derivative) < 1e-6) {
            break;
        }
        const double delta = (current_x - x) / derivative;
        u -= delta;
        if (u < 0.0) {
            u = 0.0;
        } else if (u > 1.0) {
            u = 1.0;
        }
        if (fabs(delta) < 1e-6) {
            break;
        }
    }

    const double one_minus_u = 1.0 - u;
    return 3 * one_minus_u * one_minus_u * u * curve->y1 +
           3 * one_minus_u * u * u * curve->y2 +
           u * u * u;
}

static double animation_ease(int curve_id, double t) {
    const bezier_def *curve = animation_curve(curve_id);
    if (curve->x1 == curve->y1 && curve->x2 == curve->y2) {
        /* linear */
        return t;
    }
    return bezier_eval(curve, t);
}

/******************************************************************************
 * Opacity
 ******************************************************************************/

static void ensure_atoms(void) {
    if (atom_opacity != XCB_NONE) {
        return;
    }
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(
        conn, 0, strlen("_NET_WM_WINDOW_OPACITY"), "_NET_WM_WINDOW_OPACITY");
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, NULL);
    if (reply == NULL) {
        return;
    }
    atom_opacity = reply->atom;
    free(reply);
}

static double clamp_opacity(double opacity) {
    if (opacity < 0.0) {
        return 0.0;
    }
    if (opacity > 1.0) {
        return 1.0;
    }
    return opacity;
}

void animation_set_opacity(Con *con, double opacity) {
    if (con == NULL || con->window == NULL) {
        return;
    }
    ensure_atoms();
    if (atom_opacity == XCB_NONE) {
        return;
    }

    opacity = clamp_opacity(opacity);
    uint32_t value = (uint32_t)(opacity * (double)UINT32_MAX);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, con->window->id, atom_opacity,
                        XCB_ATOM_CARDINAL, 32, 1, &value);
    /* Hyprland applies the opacity to the whole window, so apply it to the
     * frame (border and title bar) as well. */
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, con->frame.id, atom_opacity,
                        XCB_ATOM_CARDINAL, 32, 1, &value);
}

static double target_opacity(Con *con) {
    const bool is_focused = (con == focused);
    double opacity = is_focused ? config.active_opacity : config.inactive_opacity;
    if (!is_focused && config.dim_inactive) {
        opacity *= (1.0 - config.dim_strength);
    }
    return clamp_opacity(opacity);
}

void animation_apply_opacity(Con *con) {
    if (con == NULL || con->window == NULL) {
        return;
    }
    if (config.active_opacity >= 0.999 && config.inactive_opacity >= 0.999 &&
        !config.dim_inactive) {
        /* Nothing configured, keep the window fully opaque. */
        return;
    }
    animation_set_opacity(con, target_opacity(con));
}

void animation_focus_changed(Con *old, Con *new) {
    if (!config.animations && !config.dim_inactive) {
        return;
    }
    animation_apply_opacity(old);
    animation_apply_opacity(new);
}

/******************************************************************************
 * Geometry animation
 ******************************************************************************/

static void animation_start_timer(void) {
    if (!timer_started) {
        ev_timer_again(main_loop, &anim_timer);
        timer_started = true;
    }
}

static void animation_tick_cb(EV_P_ ev_timer *w, int revents) {
    const double frame_start = ev_now(EV_A);
    double now = frame_start;
    /* Only a change of the interpolated geometry requires a full tree render.
     * Opacity changes are applied directly through window properties and are
     * visible without re-rendering the layout, so pure fade animations must
     * not trigger the (comparatively expensive) tree_render() every frame. */
    bool geometry_changed = false;

    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        con_anim *anim = &(con->anim);
        if (!anim->active) {
            continue;
        }

        const double t = (anim->duration > 0.0)
                             ? (now - anim->start) / anim->duration
                             : 1.0;

        /* written this way so that a NaN timestamp finishes the animation
         * instead of producing bogus coordinates */
        if (!(t < 1.0)) {
            geometry_changed |= !anim_rect_equals(anim->cur, anim->to);
            anim->cur = anim->to;
            anim->active = false;
            if (anim->fade) {
                animation_set_opacity(con, anim->opacity_to);
            }
            continue;
        }

        double eased = animation_ease(anim->curve, t < 0.0 ? 0.0 : t);
        if (!isfinite(eased)) {
            eased = 1.0;
        }
        anim_rect cur = {
            .x = (int32_t)lround(anim->from.x + (anim->to.x - anim->from.x) * eased),
            .y = (int32_t)lround(anim->from.y + (anim->to.y - anim->from.y) * eased),
            .width = (int32_t)lround(anim->from.width + (anim->to.width - anim->from.width) * eased),
            .height = (int32_t)lround(anim->from.height + (anim->to.height - anim->from.height) * eased),
        };
        geometry_changed |= !anim_rect_equals(cur, anim->cur);
        anim->cur = cur;
        if (anim->fade) {
            animation_set_opacity(con, anim->opacity_from +
                                          (anim->opacity_to - anim->opacity_from) * eased);
        }
    }

    if (geometry_changed) {
        tree_render();
    }

    if (!animation_running()) {
        ev_timer_stop(main_loop, &anim_timer);
        timer_started = false;
    } else {
        /* Keep a steady 60 Hz cadence: a slow render shortens the sleep
         * instead of pushing every subsequent frame back by that amount. */
        const double spent = ev_now(EV_A) - frame_start;
        w->repeat = MAX(0.001, 1.0 / 60.0 - spent);
        ev_timer_again(main_loop, w);
    }
}

void animation_init(void) {
    ev_timer_init(&anim_timer, animation_tick_cb, 0.0, 1.0 / 60.0);
    anim_timer.data = NULL;
    custom_beziers_count = 0;
}

bool animation_running(void) {
    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        if (con->anim.active) {
            return true;
        }
    }
    return false;
}

anim_rect animation_rect_from_layout(Rect rect) {
    return (anim_rect){
        (int32_t)rect.x,
        (int32_t)rect.y,
        (int32_t)rect.width,
        (int32_t)rect.height,
    };
}

static bool anim_rect_equals(anim_rect a, anim_rect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

void animation_begin(Con *con, anim_rect from, anim_rect to, const char *curve, double duration, bool fade) {
    if (con == NULL || con->window == NULL || !config.animations) {
        return;
    }

    if (duration <= 0.0 || anim_rect_equals(from, to)) {
        con->anim.active = false;
        return;
    }

    con_anim *anim = &(con->anim);
    anim->active = true;
    anim->from = from;
    anim->to = to;
    anim->cur = from;
    anim->start = ev_now(main_loop);
    anim->duration = duration;
    anim->curve = animation_curve_id(curve);
    anim->fade = fade;
    anim->opacity_from = fade ? 0.0 : 1.0;
    anim->opacity_to = target_opacity(con);
    animation_start_timer();
}

void animation_slide_in(Con *workspace, int dx) {
    if (workspace == NULL || !config.animations || dx == 0) {
        return;
    }

    Con *con;
    TAILQ_FOREACH (con, &all_cons, all_cons) {
        if (con->type != CT_CON || con->window == NULL) {
            continue;
        }
        if (con_get_workspace(con) != workspace) {
            continue;
        }
        con->anim.slide_pending = true;
        con->anim.slide_dx = dx;
    }
}
