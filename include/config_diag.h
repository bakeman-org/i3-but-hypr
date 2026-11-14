/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * config_diag.h: structured collection of configuration problems.
 *
 * Both the config parser and the i3ext Lua validation record every problem
 * here, so that
 *   - the i3-nagbar can show *where* the first error is, and
 *   - `i3 -C` can print a detailed report with source context and hints.
 *
 */
#pragma once

#include <stdbool.h>
#include <stdio.h>

struct config_diag {
    char *file;
    int line;
    /* One-line reason, e.g. "Expected one of these tokens: <end>, '[', ...". */
    char *message;
    /* How to fix it; may be NULL. Multi-line text is allowed. */
    char *hint;
};

/* Forgets all recorded problems (called at the start of every config load). */
void config_diag_reset(void);

/* Records one problem. `file` may be NULL, `line` may be 0 (unknown). */
void config_diag_add(const char *file, int line, const char *message, const char *hint);

int config_diag_count(void);
const struct config_diag *config_diag_get(int index);

/* Returns a malloc'd one-line summary of the first problem, or NULL. */
char *config_diag_summary(void);

/* Prints the full report (used by `i3 -C`). `color` enables ANSI colors. */
void config_diag_print(FILE *stream, bool color);

/* Hint heuristics for the two sources of problems. Both return a static or
 * freshly allocated string; the caller duplicates it. */
const char *config_diag_hint_parse(const char *source_line, const char *message);
const char *config_diag_hint_i3ext(const char *message);
