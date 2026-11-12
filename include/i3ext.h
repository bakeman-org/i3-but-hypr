/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * i3ext.h: embedded Lua configuration block (i3ext { ... }).
 *
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * One top-level `i3ext { ... }` block as found by the Lua-aware scanner.
 *
 * The offsets refer to the buffer that was scanned and are only used
 * internally by the preprocessor; the config parser only needs the line
 * numbers.
 */
typedef struct {
    /* 1-based line number of the `i3ext {` line. */
    int start_line;
    /* 1-based line number of the matching `}` line. */
    int end_line;
    /* Offset of the first character of the `i3ext` line. */
    size_t start_offset;
    /* Offset of the opening `{`. */
    size_t open_offset;
    /* Offset of the matching `}`. */
    size_t close_offset;
} I3ExtLineRange;

/**
 * Frees all registered Lua chunks and resets the id counter. Called once
 * before parsing a configuration file (the ids are handed out while the
 * file and its includes are parsed).
 */
void i3ext_registry_reset(void);

/**
 * Scans `contents` for top-level `i3ext { ... }` blocks.
 *
 * Returns false (and logs an error including `filename` and the line number)
 * when a block is malformed, unterminated or nested inside a native
 * `bar`/`mode` block. On success, `*ranges` points to an allocated array of
 * `*num_ranges` entries (may be NULL/0) which the caller must free().
 */
bool i3ext_find_blocks(const char *contents, const char *filename,
                       I3ExtLineRange **ranges, size_t *num_ranges);

/**
 * Replaces every top-level `i3ext { ... }` block in `contents` with an
 * internal `__i3ext_exec <id>` marker, leaving all other lines and the total
 * number of lines untouched. The Lua source of each block is registered for
 * later execution via i3ext_run().
 *
 * Returns an allocated buffer (caller frees) or NULL when scanning fails.
 * `*has_errors` is set accordingly.
 */
char *i3ext_preprocess(const char *contents, const char *filename,
                       bool *has_errors);

/**
 * Executes the chunk registered under `index` in a fresh, sandboxed Lua 5.4
 * state. Returns false (after logging) on load or runtime errors.
 */
bool i3ext_run(long index);

/* Used by the scanner unit test. Returns the number of registered chunks. */
size_t i3ext_registry_count(void);

/* Used by the scanner unit test. Returns the source of a registered chunk
 * (with the leading line-alignment newlines) and its first line number. */
const char *i3ext_chunk_source(size_t index, int *start_line);
