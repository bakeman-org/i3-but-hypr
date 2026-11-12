/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * gaps.c: gaps logic: whether to display gaps at all, and how big
 *         they should be.
 *
 */
#include "all.h"

/*
 * render_con() asks for the effective gaps of every container in the tree, and
 * calculating them walks the whole workspace (con_num_visible_children()) each
 * time. That made a render pass quadratic in the number of containers, which
 * is expensive because renders happen at animation frame rate. During one
 * render pass the tree, the focus and the configuration cannot change, so the
 * result is cached per workspace. The cache is enabled only while a render
 * pass is running (see gaps_render_cache_begin()).
 */
static struct {
    bool active;
    bool valid;
    Con *workspace;
    gaps_t gaps;

    /* Cached fullscreen lookup for the workspace of the last query, used by
     * gaps_has_adjacent_container(). */
    Con *fullscreen_workspace;
    Con *fullscreen;
} gaps_cache;

void gaps_render_cache_begin(void) {
    gaps_cache.active = true;
    gaps_cache.valid = false;
    gaps_cache.workspace = NULL;
    gaps_cache.fullscreen_workspace = NULL;
    gaps_cache.fullscreen = NULL;
}

void gaps_render_cache_end(void) {
    gaps_cache.active = false;
}

/**
 * Calculates the effective gap sizes for a container.
 */
gaps_t calculate_effective_gaps(Con *con) {
    Con *workspace = con_get_workspace(con);
    if (workspace == NULL) {
        return (gaps_t){0, 0, 0, 0, 0};
    }

    if (gaps_cache.active && gaps_cache.valid && gaps_cache.workspace == workspace) {
        return gaps_cache.gaps;
    }

    bool one_child = con_num_visible_children(workspace) <= 1 ||
                     (con_num_children(workspace) == 1 &&
                      (TAILQ_FIRST(&(workspace->nodes_head))->layout == L_TABBED ||
                       TAILQ_FIRST(&(workspace->nodes_head))->layout == L_STACKED));

    gaps_t gaps = {0, 0, 0, 0, 0};
    if (!(config.smart_gaps == SMART_GAPS_ON && one_child)) {
        gaps.inner = workspace->gaps.inner + config.gaps.inner;
        if (config.smart_gaps != SMART_GAPS_INVERSE_OUTER || one_child) {
            gaps.top = workspace->gaps.top + config.gaps.top;
            gaps.right = workspace->gaps.right + config.gaps.right;
            gaps.bottom = workspace->gaps.bottom + config.gaps.bottom;
            gaps.left = workspace->gaps.left + config.gaps.left;
        }
    }

    if (gaps_cache.active) {
        gaps_cache.valid = true;
        gaps_cache.workspace = workspace;
        gaps_cache.gaps = gaps;
    }

    return gaps;
}

/*
 * Decides whether the container should be inset.
 */
bool gaps_should_inset_con(Con *con, int children) {
    /* No parent? None of the conditionals below can be true. */
    if (con->parent == NULL) {
        return false;
    }

    /* Floating split containers should never have gaps inside them. */
    if (con_inside_floating(con)) {
        return false;
    }

    const bool leaf_or_stacked_tabbed =
        con_is_leaf(con) ||
        (con->layout == L_STACKED || con->layout == L_TABBED);

    /* Inset direct children of the workspace that are leaf containers or
       stacked/tabbed containers. */
    if (leaf_or_stacked_tabbed &&
        con->parent->type == CT_WORKSPACE) {
        return true;
    }

    /* Inset direct children of vertical or horizontal split containers at any
       depth in the tree. Do not inset as soon as any parent is a stacked or
       tabbed container, to avoid double insets. */
    if (leaf_or_stacked_tabbed &&
        !con_inside_stacked_or_tabbed(con) &&
        con->parent->type == CT_CON &&
        (con->parent->layout == L_SPLITH ||
         con->parent->layout == L_SPLITV ||
         con->parent->layout == L_DWINDLE ||
         con->parent->layout == L_MASTER ||
         con->parent->layout == L_SCROLLER)) {
        return true;
    }

    return false;
}

/*
 * Returns whether the given container has an adjacent container in the
 * specified direction. In other words, this returns true if and only if
 * the container is not touching the edge of the screen in that direction.
 */
bool gaps_has_adjacent_container(Con *con, direction_t direction) {
    Con *workspace = con_get_workspace(con);
    Con *fullscreen;

    if (gaps_cache.active) {
        if (gaps_cache.fullscreen_workspace != workspace) {
            gaps_cache.fullscreen_workspace = workspace;
            gaps_cache.fullscreen = NULL;
            if (workspace != NULL) {
                gaps_cache.fullscreen = con_get_fullscreen_con(workspace, CF_GLOBAL);
                if (gaps_cache.fullscreen == NULL) {
                    gaps_cache.fullscreen = con_get_fullscreen_con(workspace, CF_OUTPUT);
                }
            }
        }
        fullscreen = gaps_cache.fullscreen;
    } else {
        fullscreen = con_get_fullscreen_con(workspace, CF_GLOBAL);
        if (fullscreen == NULL) {
            fullscreen = con_get_fullscreen_con(workspace, CF_OUTPUT);
        }
    }

    /* If this container is fullscreen by itself, there's no adjacent container. */
    if (con == fullscreen) {
        return false;
    }

    Con *first = con;
    Con *second = NULL;
    bool found_neighbor = resize_find_tiling_participants(&first, &second, direction, false);
    if (!found_neighbor) {
        return false;
    }

    /* If we have an adjacent container and nothing is fullscreen, we consider it. */
    if (fullscreen == NULL) {
        return true;
    }

    /* For fullscreen containers, only consider the adjacent container if it is also fullscreen. */
    return con_has_parent(con, fullscreen) && con_has_parent(second, fullscreen);
}

/*
 * Returns the configured gaps for this workspace based on the workspace name,
 * number, and configured workspace gap assignments.
 */
gaps_t gaps_for_workspace(Con *ws) {
    gaps_t gaps = (gaps_t){0, 0, 0, 0, 0};
    gaps_mask_t mask = 0;
    struct Workspace_Assignment *assignment;
    TAILQ_FOREACH (assignment, &ws_assignments, ws_assignments) {
        if (strcmp(assignment->name, ws->name) == 0) {
            gaps = assignment->gaps;
            mask = assignment->gaps_mask;
            break;
        } else if (ws->num != -1 && name_is_digits(assignment->name) && ws_name_to_number(assignment->name) == ws->num) {
            gaps = assignment->gaps;
            mask = assignment->gaps_mask;
        }
    }
    if (mask == 0) {
        return gaps;
    }

    if (mask & GAPS_INNER) {
        gaps.inner -= config.gaps.inner;
    }
    if (mask & GAPS_TOP) {
        gaps.top -= config.gaps.top;
    }
    if (mask & GAPS_RIGHT) {
        gaps.right -= config.gaps.right;
    }
    if (mask & GAPS_BOTTOM) {
        gaps.bottom -= config.gaps.bottom;
    }
    if (mask & GAPS_LEFT) {
        gaps.left -= config.gaps.left;
    }

    return gaps;
}

/*
 * Re-applies all workspace gap assignments to existing workspaces after
 * reloading the configuration file.
 *
 */
void gaps_reapply_workspace_assignments(void) {
    Con *output, *workspace = NULL;
    TAILQ_FOREACH (output, &(croot->nodes_head), nodes) {
        Con *content = output_get_content(output);
        TAILQ_FOREACH (workspace, &(content->nodes_head), nodes) {
            DLOG("updating gap assignments for workspace %s\n", workspace->name);
            workspace->gaps = gaps_for_workspace(workspace);
        }
    }
}
