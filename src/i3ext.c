/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * i3ext.c: top-level `i3ext { ... }` configuration blocks.
 *
 * The block contents are Lua 5.4. The config parser hands us the raw file
 * (Pass A, for line ranges) and the variable-replaced contents (Pass B, for
 * extracting and registering the Lua source). Each block is later executed
 * in its original position via the internal `__i3ext_exec <id>` marker.
 *
 */
#include "all.h"

#include <ctype.h>

#ifndef I3EXT_SCAN_TEST
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#endif

#include "i3ext.h"

#define I3EXT_NPOS ((size_t)-1)

/*******************************************************************************
 * Lua-aware scanner.
 ******************************************************************************/

static size_t i3ext_line_start(const char *s, size_t pos) {
    while (pos > 0 && s[pos - 1] != '\n') {
        pos--;
    }
    return pos;
}

static size_t i3ext_line_end(const char *s, size_t len, size_t pos) {
    while (pos < len && s[pos] != '\n' && s[pos] != '\r') {
        pos++;
    }
    return pos;
}

static size_t i3ext_count_newlines(const char *s, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            n++;
        }
    }
    return n;
}

/* Returns true if s[pos] starts a Lua long bracket `[=*[`, with the number of
 * '=' characters stored in *eq. */
static bool i3ext_long_bracket_open(const char *s, size_t len, size_t pos, int *eq) {
    if (pos >= len || s[pos] != '[') {
        return false;
    }
    size_t p = pos + 1;
    int n = 0;
    while (p < len && s[p] == '=') {
        n++;
        p++;
    }
    if (p < len && s[p] == '[') {
        *eq = n;
        return true;
    }
    return false;
}

/* Skips a long bracket whose contents start at `content`. Returns the offset
 * just after the closing bracket, or I3EXT_NPOS if it is not terminated. */
static size_t i3ext_long_bracket_skip(const char *s, size_t len, size_t content, int eq) {
    size_t p = content;
    while (p < len) {
        if (s[p] == ']') {
            size_t q = p + 1;
            int n = 0;
            while (q < len && s[q] == '=') {
                n++;
                q++;
            }
            if (n == eq && q < len && s[q] == ']') {
                return q + 1;
            }
        }
        p++;
    }
    return I3EXT_NPOS;
}

/* Skips a single- or double-quoted Lua string. */
static size_t i3ext_short_string_skip(const char *s, size_t len, size_t pos) {
    const char quote = s[pos];
    pos++;
    while (pos < len) {
        if (s[pos] == '\\') {
            pos += (pos + 1 < len) ? 2 : 1;
            continue;
        }
        if (s[pos] == quote) {
            return pos + 1;
        }
        pos++;
    }
    return I3EXT_NPOS;
}

/* Given the offset of the opening '{', returns the offset of the matching '}'
 * or I3EXT_NPOS if the block is not terminated. Lua strings, short/long
 * comments and nested braces are understood. */
static size_t i3ext_scan_block(const char *s, size_t len, size_t open_offset) {
    size_t i = open_offset + 1;
    int depth = 1;

    while (i < len) {
        const char c = s[i];

        if (c == '-' && i + 1 < len && s[i + 1] == '-') {
            int eq = 0;
            if (i3ext_long_bracket_open(s, len, i + 2, &eq)) {
                const size_t end = i3ext_long_bracket_skip(s, len, i + eq + 4, eq);
                if (end == I3EXT_NPOS) {
                    return I3EXT_NPOS;
                }
                i = end;
                continue;
            }
            while (i < len && s[i] != '\n') {
                i++;
            }
            continue;
        }

        if (c == '[') {
            int eq = 0;
            if (i3ext_long_bracket_open(s, len, i, &eq)) {
                const size_t end = i3ext_long_bracket_skip(s, len, i + eq + 2, eq);
                if (end == I3EXT_NPOS) {
                    return I3EXT_NPOS;
                }
                i = end;
                continue;
            }
            i++;
            continue;
        }

        if (c == '"' || c == '\'') {
            const size_t end = i3ext_short_string_skip(s, len, i);
            if (end == I3EXT_NPOS) {
                return I3EXT_NPOS;
            }
            i = end;
            continue;
        }

        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                return i;
            }
        }
        i++;
    }

    return I3EXT_NPOS;
}

static bool i3ext_token_is(const char *s, size_t start, size_t end, const char *name) {
    const size_t n = strlen(name);
    return (end - start) == n && memcmp(s + start, name, n) == 0;
}

static void i3ext_count_braces(const char *s, size_t start, size_t end, int *opens, int *closes) {
    /* A full-line comment cannot contain structural braces. A '#' elsewhere
     * is either a color prefix (bar colors) or part of a command, so it must
     * not hide the closing braces on the same line. */
    size_t first = start;
    while (first < end && (s[first] == ' ' || s[first] == '\t')) {
        first++;
    }
    if (first < end && s[first] == '#') {
        return;
    }

    bool in_single = false;
    bool in_double = false;
    for (size_t i = start; i < end; i++) {
        const char c = s[i];
        if (in_single) {
            if (c == '\\' && i + 1 < end) {
                i++;
            } else if (c == '\'') {
                in_single = false;
            }
            continue;
        }
        if (in_double) {
            if (c == '\\' && i + 1 < end) {
                i++;
            } else if (c == '"') {
                in_double = false;
            }
            continue;
        }
        if (c == '\'') {
            in_single = true;
        } else if (c == '"') {
            in_double = true;
        } else if (c == '{') {
            (*opens)++;
        } else if (c == '}') {
            (*closes)++;
        }
    }
}

bool i3ext_find_blocks(const char *contents, const char *filename,
                       I3ExtLineRange **ranges, size_t *num_ranges) {
    *ranges = NULL;
    *num_ranges = 0;

    const size_t len = strlen(contents);
    size_t capacity = 0;
    int native_depth = 0;
    size_t pos = 0;
    int line = 1;

    while (pos < len) {
        const size_t line_start = pos;
        const size_t line_end = i3ext_line_end(contents, len, pos);
        size_t next = line_end;
        if (next < len && contents[next] == '\r') {
            next++;
        }
        if (next < len && contents[next] == '\n') {
            next++;
        }

        size_t token = line_start;
        while (token < line_end && (contents[token] == ' ' || contents[token] == '\t')) {
            token++;
        }
        size_t token_end = token;
        while (token_end < line_end && contents[token_end] != ' ' &&
               contents[token_end] != '\t' && contents[token_end] != '{' &&
               contents[token_end] != '}') {
            token_end++;
        }

        const bool is_i3ext = i3ext_token_is(contents, token, token_end, "i3ext");

        if (native_depth > 0) {
            if (is_i3ext) {
                ELOG("%s:%d: i3ext blocks must be at the top level (not inside bar/mode)\n",
                     filename, line);
                goto fail;
            }
            int opens = 0, closes = 0;
            i3ext_count_braces(contents, line_start, line_end, &opens, &closes);
            native_depth += opens - closes;
            if (native_depth < 0) {
                native_depth = 0;
            }
            pos = next;
            line++;
            continue;
        }

        if (i3ext_token_is(contents, token, token_end, "bar") ||
            i3ext_token_is(contents, token, token_end, "mode")) {
            int opens = 0, closes = 0;
            i3ext_count_braces(contents, line_start, line_end, &opens, &closes);
            if (opens > 0) {
                native_depth = opens - closes;
                if (native_depth < 0) {
                    native_depth = 0;
                }
            }
            pos = next;
            line++;
            continue;
        }

        if (is_i3ext) {
            size_t p = token + strlen("i3ext");
            while (p < line_end && (contents[p] == ' ' || contents[p] == '\t')) {
                p++;
            }
            if (p >= line_end || contents[p] != '{') {
                ELOG("%s:%d: i3ext: expected '{' at end of line\n", filename, line);
                goto fail;
            }
            size_t q = p + 1;
            while (q < line_end && (contents[q] == ' ' || contents[q] == '\t')) {
                q++;
            }
            if (q != line_end) {
                ELOG("%s:%d: i3ext: opening '{' must be the last token on the line\n",
                     filename, line);
                goto fail;
            }

            const size_t close = i3ext_scan_block(contents, len, p);
            if (close == I3EXT_NPOS) {
                ELOG("%s:%d: i3ext: unterminated block (missing '}')\n", filename, line);
                goto fail;
            }

            /* The closing brace must be on its own line. */
            const size_t close_line_start = i3ext_line_start(contents, close);
            const size_t close_line_end = i3ext_line_end(contents, len, close);
            bool bad = false;
            for (size_t z = close_line_start; z < close; z++) {
                if (contents[z] != ' ' && contents[z] != '\t') {
                    bad = true;
                    break;
                }
            }
            size_t z = close + 1;
            while (z < close_line_end && (contents[z] == ' ' || contents[z] == '\t')) {
                z++;
            }
            if (z < close_line_end &&
                !(z + 1 < close_line_end && contents[z] == '-' && contents[z + 1] == '-')) {
                bad = true;
            }
            if (bad) {
                ELOG("%s:%d: i3ext: closing '}' must be on its own line\n", filename, line);
                goto fail;
            }

            if (*num_ranges == capacity) {
                capacity = capacity ? capacity * 2 : 4;
                *ranges = realloc(*ranges, capacity * sizeof(I3ExtLineRange));
                if (*ranges == NULL) {
                    ELOG("Out of memory while scanning i3ext blocks\n");
                    return false;
                }
            }

            I3ExtLineRange *range = &(*ranges)[*num_ranges];
            range->start_line = line;
            range->end_line = line + (int)i3ext_count_newlines(contents + line_start,
                                                               close - line_start);
            range->start_offset = line_start;
            range->open_offset = p;
            range->close_offset = close;
            (*num_ranges)++;

            /* Continue after the closing line. */
            line = range->end_line + 1;
            pos = close_line_end;
            if (pos < len && contents[pos] == '\r') {
                pos++;
            }
            if (pos < len && contents[pos] == '\n') {
                pos++;
            }
            continue;
        }

        pos = next;
        line++;
    }

    return true;

fail:
    free(*ranges);
    *ranges = NULL;
    *num_ranges = 0;
    return false;
}

/*******************************************************************************
 * Registry of Lua chunks.
 ******************************************************************************/

struct i3ext_chunk {
    long id;
    char *source;
    int start_line;
    char *filename;
    struct i3ext_chunk *next;
};

static struct i3ext_chunk *i3ext_chunks = NULL;
static long i3ext_next_id = 0;

void i3ext_registry_reset(void) {
    while (i3ext_chunks != NULL) {
        struct i3ext_chunk *next = i3ext_chunks->next;
        FREE(i3ext_chunks->source);
        FREE(i3ext_chunks->filename);
        FREE(i3ext_chunks);
        i3ext_chunks = next;
    }
    i3ext_next_id = 0;
}

size_t i3ext_registry_count(void) {
    size_t n = 0;
    for (struct i3ext_chunk *c = i3ext_chunks; c != NULL; c = c->next) {
        n++;
    }
    return n;
}

const char *i3ext_chunk_source(size_t index, int *start_line) {
    size_t i = 0;
    for (struct i3ext_chunk *c = i3ext_chunks; c != NULL; c = c->next, i++) {
        if (i != index) {
            continue;
        }
        if (start_line != NULL) {
            *start_line = c->start_line;
        }
        return c->source;
    }
    return NULL;
}

static long i3ext_register_chunk(const char *source, size_t source_len,
                                 int start_line, const char *filename) {
    struct i3ext_chunk *chunk = scalloc(1, sizeof(struct i3ext_chunk));
    chunk->id = i3ext_next_id++;

    /* Prepend just enough newlines so that Lua's line numbers match the
     * physical line numbers in the configuration file. The chunk starts on
     * `start_line`, so the first source character must be on that line. */
    const size_t padding = (start_line > 0) ? (size_t)(start_line - 1) : 0;
    chunk->source = scalloc(padding + source_len + 1, 1);
    memset(chunk->source, '\n', padding);
    memcpy(chunk->source + padding, source, source_len);
    chunk->start_line = start_line;
    chunk->filename = sstrdup(filename);

    /* Keep the registration order stable. */
    struct i3ext_chunk **tail = &i3ext_chunks;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    *tail = chunk;

    DLOG("i3ext: registered block %ld at %s:%d\n", chunk->id, filename, start_line);
    return chunk->id;
}

struct i3ext_buffer {
    char *data;
    size_t len;
    size_t capacity;
};

static void i3ext_buffer_init(struct i3ext_buffer *buffer) {
    buffer->capacity = 256;
    buffer->data = smalloc(buffer->capacity);
    buffer->len = 0;
}

static void i3ext_buffer_add(struct i3ext_buffer *buffer, const char *data, size_t len) {
    while (buffer->len + len + 1 > buffer->capacity) {
        buffer->capacity *= 2;
        buffer->data = realloc(buffer->data, buffer->capacity);
        if (buffer->data == NULL) {
            die("Out of memory while preprocessing i3ext blocks");
        }
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
}

char *i3ext_preprocess(const char *contents, const char *filename, bool *has_errors) {
    *has_errors = false;

    I3ExtLineRange *ranges = NULL;
    size_t num_ranges = 0;
    if (!i3ext_find_blocks(contents, filename, &ranges, &num_ranges)) {
        *has_errors = true;
        return NULL;
    }

    const size_t len = strlen(contents);
    struct i3ext_buffer buffer;
    i3ext_buffer_init(&buffer);

    size_t pos = 0;
    size_t range_idx = 0;
    int line = 1;

    while (pos < len) {
        const size_t line_start = pos;
        const size_t line_end = i3ext_line_end(contents, len, pos);
        size_t next = line_end;
        if (next < len && contents[next] == '\r') {
            next++;
        }
        if (next < len && contents[next] == '\n') {
            next++;
        }

        if (range_idx < num_ranges && line == ranges[range_idx].start_line) {
            const I3ExtLineRange *range = &ranges[range_idx];
            const size_t source_len = range->close_offset - range->start_offset + 1;
            const long id = i3ext_register_chunk(contents + range->start_offset,
                                                 source_len,
                                                 range->start_line,
                                                 filename);

            char marker[64];
            const int marker_len = snprintf(marker, sizeof(marker), "__i3ext_exec %ld", id);
            i3ext_buffer_add(&buffer, marker, (size_t)marker_len);
            /* Preserve the newline sequence of the replaced line. */
            i3ext_buffer_add(&buffer, contents + line_end, next - line_end);
            range_idx++;
        } else if (range_idx > 0 && line > ranges[range_idx - 1].start_line &&
                   line <= ranges[range_idx - 1].end_line) {
            /* Blank out the rest of the block, keeping the line count. */
            i3ext_buffer_add(&buffer, contents + line_end, next - line_end);
        } else {
            i3ext_buffer_add(&buffer, contents + line_start, next - line_start);
        }

        pos = next;
        line++;
    }

    free(ranges);

    if (buffer.len == 0) {
        /* Contents were empty; make sure we return a valid C string. */
        i3ext_buffer_add(&buffer, "", 0);
    }
    return buffer.data;
}

#ifndef I3EXT_SCAN_TEST

/*******************************************************************************
 * Schema validation and application.
 ******************************************************************************/

static const char *const i3ext_top_keys[] = {
    "binds", "theme", "themes", "bar", "gaps", "decoration", "opacity",
    "titlebar", "client", "switcher", "animations", "dropdown", "layouts",
    "status_line", "hot_reload", "key_hints"};

static const char *const i3ext_bind_keys[] = {"key", "command", "release", "type"};
static const char *const i3ext_bind_types[] = {"bindsym", "bindcode"};

static const char *const i3ext_gap_keys[] = {
    "inner", "outer", "horizontal", "vertical", "top", "right", "bottom", "left"};

static const char *const i3ext_decoration_keys[] = {"rounding", "border_size"};
static const char *const i3ext_opacity_keys[] = {"active", "inactive", "dim_inactive", "dim_strength"};
static const char *const i3ext_titlebar_keys[] = {"align", "height", "padding", "font", "visibility"};
static const char *const i3ext_client_keys[] = {"background", "colors", "gradients"};
static const char *const i3ext_client_states[] = {"focused", "focused_inactive", "unfocused", "urgent"};
static const char *const i3ext_switcher_keys[] = {"background", "bar", "tile"};
static const char *const i3ext_theme_keys[] = {"client", "bar", "switcher"};
static const char *const i3ext_bar_single_keys[] = {
    "background", "statusline", "separator",
    "focused_background", "focused_statusline", "focused_separator"};
static const char *const i3ext_bar_triple_keys[] = {
    "focused_workspace", "active_workspace", "inactive_workspace",
    "urgent_workspace", "binding_mode"};
static const char *const i3ext_bar_keys[] = {
    "background", "statusline", "separator",
    "focused_background", "focused_statusline", "focused_separator",
    "focused_workspace", "active_workspace", "inactive_workspace",
    "urgent_workspace", "binding_mode"};
static const char *const i3ext_animations_keys[] = {
    "enabled", "beziers", "windows", "open", "close", "fade_in",
    "workspaces", "dropdown"};
static const char *const i3ext_anim_type_keys[] = {"duration", "curve", "enabled"};
static const char *const i3ext_dropdown_keys[] = {"height", "position"};
static const char *const i3ext_layouts_keys[] = {"master", "dwindle", "scroller"};
static const char *const i3ext_master_keys[] = {"ratio", "orientation"};
static const char *const i3ext_dwindle_keys[] = {"split_ratio"};
static const char *const i3ext_scroller_keys[] = {
    "proportion", "structs", "focus_center", "prefer_center",
    "prefer_overspread", "ignore_proportion_single",
    "default_proportion_single", "autofit"};

#define I3EXT_ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

static void i3ext_check_keys(lua_State *L, int idx, const char *path,
                             const char *const *keys, size_t num_keys) {
    idx = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "%s: field names must be strings", path);
        }
        const char *key = lua_tostring(L, -2);
        bool known = false;
        for (size_t i = 0; i < num_keys; i++) {
            if (strcmp(key, keys[i]) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            luaL_error(L, "%s.%s: unknown field", path, key);
        }
        lua_pop(L, 1);
    }
}

static bool i3ext_get_table(lua_State *L, int idx, const char *key, const char *path) {
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    if (!lua_istable(L, -1)) {
        luaL_error(L, "%s: expected table", path);
    }
    return true;
}

static bool i3ext_get_number(lua_State *L, int idx, const char *key, const char *path,
                             double min, double max, double *out) {
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    if (!lua_isnumber(L, -1)) {
        luaL_error(L, "%s: expected number", path);
    }
    const double value = lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (value < min || value > max) {
        luaL_error(L, "%s: value %f out of range [%f, %f]", path, value, min, max);
    }
    *out = value;
    return true;
}

static bool i3ext_get_int(lua_State *L, int idx, const char *key, const char *path,
                          long min, long max, long *out) {
    double value = 0;
    if (!i3ext_get_number(L, idx, key, path, (double)min, (double)max, &value)) {
        return false;
    }
    if (value != (double)(long)value) {
        luaL_error(L, "%s: expected integer", path);
    }
    *out = (long)value;
    return true;
}

static bool i3ext_get_bool(lua_State *L, int idx, const char *key, const char *path,
                           bool *out) {
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    if (lua_type(L, -1) != LUA_TBOOLEAN) {
        luaL_error(L, "%s: expected boolean", path);
    }
    *out = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return true;
}

static bool i3ext_get_string(lua_State *L, int idx, const char *key, const char *path,
                             const char **out) {
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    if (lua_type(L, -1) != LUA_TSTRING) {
        luaL_error(L, "%s: expected string", path);
    }
    *out = lua_tostring(L, -1);
    lua_pop(L, 1);
    return true;
}

static bool i3ext_get_enum(lua_State *L, int idx, const char *key, const char *path,
                           const char *const *values, size_t num_values, int *out) {
    const char *value = NULL;
    if (!i3ext_get_string(L, idx, key, path, &value)) {
        return false;
    }
    for (size_t i = 0; i < num_values; i++) {
        if (strcmp(value, values[i]) == 0) {
            *out = (int)i;
            return true;
        }
    }
    luaL_error(L, "%s: invalid value \"%s\"", path, value);
    return false;
}

static bool i3ext_is_hex_color(const char *value) {
    const size_t len = strlen(value);
    if (len != 7 && len != 9) {
        return false;
    }
    if (value[0] != '#') {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        if (!isxdigit((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static bool i3ext_get_hex(lua_State *L, int idx, const char *key, const char *path) {
    const char *value = NULL;
    if (!i3ext_get_string(L, idx, key, path, &value)) {
        return false;
    }
    if (!i3ext_is_hex_color(value)) {
        luaL_error(L, "%s: expected hex color (#rrggbb or #rrggbbaa)", path);
    }
    return true;
}

static void i3ext_check_hex_element(lua_State *L, int idx, const char *path) {
    if (lua_type(L, idx) != LUA_TSTRING || !i3ext_is_hex_color(lua_tostring(L, idx))) {
        luaL_error(L, "%s: expected hex color (#rrggbb or #rrggbbaa)", path);
    }
}

static void i3ext_validate_color_list(lua_State *L, int idx, const char *path, int expected) {
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) {
        luaL_error(L, "%s: expected table of colors", path);
    }
    if ((int)lua_rawlen(L, idx) != expected) {
        luaL_error(L, "%s: expected %d colors", path, expected);
    }
    for (int i = 1; i <= expected; i++) {
        char element_path[256];
        snprintf(element_path, sizeof(element_path), "%s[%d]", path, i);
        lua_geti(L, idx, i);
        i3ext_check_hex_element(L, -1, element_path);
        lua_pop(L, 1);
    }
}

static void i3ext_validate_gradient(lua_State *L, int idx, const char *path) {
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) {
        luaL_error(L, "%s: expected table { start, end [, angle] }", path);
    }
    const size_t len = lua_rawlen(L, idx);
    if (len != 2 && len != 3) {
        luaL_error(L, "%s: expected 2 or 3 values (start, end [, angle])", path);
    }
    for (size_t i = 1; i <= len; i++) {
        char element_path[256];
        snprintf(element_path, sizeof(element_path), "%s[%zu]", path, i);
        lua_geti(L, idx, (lua_Integer)i);
        if (i <= 2) {
            i3ext_check_hex_element(L, -1, element_path);
        } else if (!lua_isnumber(L, -1)) {
            luaL_error(L, "%s: expected angle as number", element_path);
        }
        lua_pop(L, 1);
    }
}

static void i3ext_validate_animation_type(lua_State *L, int idx, const char *path) {
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) {
        luaL_error(L, "%s: expected table", path);
    }
    i3ext_check_keys(L, idx, path, i3ext_anim_type_keys, I3EXT_ARRAY_LEN(i3ext_anim_type_keys));

    char field_path[256];
    long duration;
    snprintf(field_path, sizeof(field_path), "%s.duration", path);
    i3ext_get_int(L, idx, "duration", field_path, 0, 600000, &duration);

    const char *curve;
    snprintf(field_path, sizeof(field_path), "%s.curve", path);
    if (i3ext_get_string(L, idx, "curve", field_path, &curve) && *curve == '\0') {
        luaL_error(L, "%s: curve name must not be empty", field_path);
    }

    bool enabled;
    snprintf(field_path, sizeof(field_path), "%s.enabled", path);
    i3ext_get_bool(L, idx, "enabled", field_path, &enabled);
}

#define I3EXT_DEFAULT_THEME "gruber_darker"

static void i3ext_validate_bar_triple(lua_State *L, int idx, const char *path) {
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) {
        luaL_error(L, "%s: expected table { border, background, text }", path);
    }
    if (lua_rawlen(L, idx) != 3) {
        luaL_error(L, "%s: expected exactly 3 colors", path);
    }
    for (int i = 1; i <= 3; i++) {
        char element_path[384];
        snprintf(element_path, sizeof(element_path), "%s[%d]", path, i);
        lua_geti(L, idx, i);
        i3ext_check_hex_element(L, -1, element_path);
        lua_pop(L, 1);
    }
}

static void i3ext_validate_bar(lua_State *L, int idx, const char *path) {
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) {
        luaL_error(L, "%s: expected table", path);
    }
    i3ext_check_keys(L, idx, path, i3ext_bar_keys, I3EXT_ARRAY_LEN(i3ext_bar_keys));

    char sub[384];
    for (size_t i = 0; i < I3EXT_ARRAY_LEN(i3ext_bar_single_keys); i++) {
        snprintf(sub, sizeof(sub), "%s.%s", path, i3ext_bar_single_keys[i]);
        i3ext_get_hex(L, idx, i3ext_bar_single_keys[i], sub);
    }
    for (size_t i = 0; i < I3EXT_ARRAY_LEN(i3ext_bar_triple_keys); i++) {
        snprintf(sub, sizeof(sub), "%s.%s", path, i3ext_bar_triple_keys[i]);
        if (i3ext_get_table(L, idx, i3ext_bar_triple_keys[i], sub)) {
            i3ext_validate_bar_triple(L, -1, sub);
            lua_pop(L, 1);
        }
    }
}

static void i3ext_validate_switcher(lua_State *L, int idx, const char *path) {
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) {
        luaL_error(L, "%s: expected table", path);
    }
    i3ext_check_keys(L, idx, path, i3ext_switcher_keys, I3EXT_ARRAY_LEN(i3ext_switcher_keys));

    char sub[384];
    snprintf(sub, sizeof(sub), "%s.background", path);
    i3ext_get_hex(L, idx, "background", sub);
    snprintf(sub, sizeof(sub), "%s.bar", path);
    if (i3ext_get_table(L, idx, "bar", sub)) {
        i3ext_validate_color_list(L, -1, sub, 2);
        lua_pop(L, 1);
    }
    snprintf(sub, sizeof(sub), "%s.tile", path);
    if (i3ext_get_table(L, idx, "tile", sub)) {
        i3ext_validate_color_list(L, -1, sub, 2);
        lua_pop(L, 1);
    }
}

static void i3ext_validate_client(lua_State *L, int idx, const char *path) {
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) {
        luaL_error(L, "%s: expected table", path);
    }
    i3ext_check_keys(L, idx, path, i3ext_client_keys, I3EXT_ARRAY_LEN(i3ext_client_keys));

    char sub[384];
    snprintf(sub, sizeof(sub), "%s.background", path);
    i3ext_get_hex(L, idx, "background", sub);

    snprintf(sub, sizeof(sub), "%s.colors", path);
    if (i3ext_get_table(L, idx, "colors", sub)) {
        const int colors = lua_absindex(L, -1);
        i3ext_check_keys(L, colors, sub, i3ext_client_states,
                         I3EXT_ARRAY_LEN(i3ext_client_states));
        for (size_t i = 0; i < I3EXT_ARRAY_LEN(i3ext_client_states); i++) {
            char state_path[420];
            snprintf(state_path, sizeof(state_path), "%s.%s", sub, i3ext_client_states[i]);
            lua_getfield(L, colors, i3ext_client_states[i]);
            if (!lua_isnil(L, -1)) {
                i3ext_validate_color_list(L, -1, state_path, 5);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    snprintf(sub, sizeof(sub), "%s.gradients", path);
    if (i3ext_get_table(L, idx, "gradients", sub)) {
        const int gradients = lua_absindex(L, -1);
        i3ext_check_keys(L, gradients, sub, i3ext_client_states,
                         I3EXT_ARRAY_LEN(i3ext_client_states));
        for (size_t i = 0; i < I3EXT_ARRAY_LEN(i3ext_client_states); i++) {
            char state_path[420];
            snprintf(state_path, sizeof(state_path), "%s.%s", sub, i3ext_client_states[i]);
            lua_getfield(L, gradients, i3ext_client_states[i]);
            if (!lua_isnil(L, -1)) {
                i3ext_validate_gradient(L, -1, state_path);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}

static void i3ext_validate_themes(lua_State *L, int idx, const char *path) {
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) {
        luaL_error(L, "%s: expected table", path);
    }
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "%s: theme names must be strings", path);
        }
        const char *name = lua_tostring(L, -2);
        if (*name == '\0') {
            luaL_error(L, "%s: theme name must not be empty", path);
        }
        if (!lua_istable(L, -1)) {
            luaL_error(L, "%s.%s: expected table", path, name);
        }
        const int theme = lua_absindex(L, -1);
        char theme_path[384];
        snprintf(theme_path, sizeof(theme_path), "%s.%s", path, name);
        i3ext_check_keys(L, theme, theme_path, i3ext_theme_keys,
                         I3EXT_ARRAY_LEN(i3ext_theme_keys));

        char sub[420];
        snprintf(sub, sizeof(sub), "%s.client", theme_path);
        if (i3ext_get_table(L, theme, "client", sub)) {
            i3ext_validate_client(L, -1, sub);
            lua_pop(L, 1);
        }
        snprintf(sub, sizeof(sub), "%s.bar", theme_path);
        if (i3ext_get_table(L, theme, "bar", sub)) {
            i3ext_validate_bar(L, -1, sub);
            lua_pop(L, 1);
        }
        snprintf(sub, sizeof(sub), "%s.switcher", theme_path);
        if (i3ext_get_table(L, theme, "switcher", sub)) {
            i3ext_validate_switcher(L, -1, sub);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}

static void i3ext_validate_theme_selection(lua_State *L, int table) {
    table = lua_absindex(L, table);

    lua_getfield(L, table, "theme");
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING || *lua_tostring(L, -1) == '\0') {
            luaL_error(L, "i3ext.theme: expected non-empty string");
        }
    }
    const char *selected = lua_isnil(L, -1) ? I3EXT_DEFAULT_THEME : lua_tostring(L, -1);
    const bool has_theme_field = !lua_isnil(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, table, "themes");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (has_theme_field) {
            luaL_error(L, "i3ext.theme: no themes are defined");
        }
        return;
    }
    if (!lua_istable(L, -1)) {
        luaL_error(L, "i3ext.themes: expected table");
    }
    const int themes = lua_absindex(L, -1);
    lua_getfield(L, themes, selected);
    if (lua_isnil(L, -1)) {
        luaL_error(L, "i3ext.theme: theme \"%s\" is not defined", selected);
    }
    lua_pop(L, 2);
}

static void i3ext_validate(lua_State *L, int table) {
    table = lua_absindex(L, table);
    if (!lua_istable(L, table)) {
        luaL_error(L, "i3ext: expected a table");
    }
    i3ext_check_keys(L, table, "i3ext", i3ext_top_keys, I3EXT_ARRAY_LEN(i3ext_top_keys));

    if (i3ext_get_table(L, table, "binds", "i3ext.binds")) {
        const int binds = lua_absindex(L, -1);
        const size_t count = lua_rawlen(L, binds);
        size_t pairs = 0;
        lua_pushnil(L);
        while (lua_next(L, binds)) {
            pairs++;
            lua_pop(L, 1);
        }
        if (pairs != count) {
            luaL_error(L, "i3ext.binds: expected a list of tables");
        }
        for (size_t i = 1; i <= count; i++) {
            char path[128];
            snprintf(path, sizeof(path), "i3ext.binds[%zu]", i);
            lua_geti(L, binds, (lua_Integer)i);
            if (!lua_istable(L, -1)) {
                luaL_error(L, "%s: expected table", path);
            }
            const int entry = lua_absindex(L, -1);
            i3ext_check_keys(L, entry, path, i3ext_bind_keys,
                             I3EXT_ARRAY_LEN(i3ext_bind_keys));

            char field_path[160];
            const char *key;
            snprintf(field_path, sizeof(field_path), "%s.key", path);
            if (!i3ext_get_string(L, entry, "key", field_path, &key) || *key == '\0') {
                luaL_error(L, "%s: expected non-empty string", field_path);
            }
            const char *last_plus = strrchr(key, '+');
            if (last_plus != NULL && last_plus[1] == '\0') {
                luaL_error(L, "%s: missing key after '+'", field_path);
            }

            const char *command;
            snprintf(field_path, sizeof(field_path), "%s.command", path);
            if (!i3ext_get_string(L, entry, "command", field_path, &command) ||
                *command == '\0') {
                luaL_error(L, "%s: expected non-empty string", field_path);
            }

            bool release;
            snprintf(field_path, sizeof(field_path), "%s.release", path);
            i3ext_get_bool(L, entry, "release", field_path, &release);

            int type = 0;
            snprintf(field_path, sizeof(field_path), "%s.type", path);
            i3ext_get_enum(L, entry, "type", field_path, i3ext_bind_types,
                           I3EXT_ARRAY_LEN(i3ext_bind_types), &type);
            if (type == 1) {
                /* For bindcode the last segment of `key` is the keycode. */
                if (last_plus == NULL) {
                    luaL_error(L, "%s: bindcode requires \"modifiers+keycode\"", field_path);
                }
                char *end = NULL;
                errno = 0;
                const long code = strtol(last_plus + 1, &end, 10);
                if (errno != 0 || end == last_plus + 1 || *end != '\0' || code <= 0) {
                    luaL_error(L, "%s: invalid keycode \"%s\"", field_path, last_plus + 1);
                }
            }

            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    if (i3ext_get_table(L, table, "gaps", "i3ext.gaps")) {
        const int gaps = lua_absindex(L, -1);
        i3ext_check_keys(L, gaps, "i3ext.gaps", i3ext_gap_keys, I3EXT_ARRAY_LEN(i3ext_gap_keys));
        for (size_t i = 0; i < I3EXT_ARRAY_LEN(i3ext_gap_keys); i++) {
            char path[128];
            long value;
            snprintf(path, sizeof(path), "i3ext.gaps.%s", i3ext_gap_keys[i]);
            i3ext_get_int(L, gaps, i3ext_gap_keys[i], path, 0, 1000, &value);
        }
        lua_pop(L, 1);
    }

    if (i3ext_get_table(L, table, "decoration", "i3ext.decoration")) {
        const int decoration = lua_absindex(L, -1);
        i3ext_check_keys(L, decoration, "i3ext.decoration",
                         i3ext_decoration_keys, I3EXT_ARRAY_LEN(i3ext_decoration_keys));
        long value;
        i3ext_get_int(L, decoration, "rounding", "i3ext.decoration.rounding", 0, 100, &value);
        i3ext_get_int(L, decoration, "border_size", "i3ext.decoration.border_size", 0, 100, &value);
        lua_pop(L, 1);
    }

    if (i3ext_get_table(L, table, "opacity", "i3ext.opacity")) {
        const int opacity = lua_absindex(L, -1);
        i3ext_check_keys(L, opacity, "i3ext.opacity",
                         i3ext_opacity_keys, I3EXT_ARRAY_LEN(i3ext_opacity_keys));
        double value;
        i3ext_get_number(L, opacity, "active", "i3ext.opacity.active", 0.0, 1.0, &value);
        i3ext_get_number(L, opacity, "inactive", "i3ext.opacity.inactive", 0.0, 1.0, &value);
        i3ext_get_number(L, opacity, "dim_strength", "i3ext.opacity.dim_strength", 0.0, 1.0, &value);
        bool enabled;
        i3ext_get_bool(L, opacity, "dim_inactive", "i3ext.opacity.dim_inactive", &enabled);
        lua_pop(L, 1);
    }

    if (i3ext_get_table(L, table, "titlebar", "i3ext.titlebar")) {
        const int titlebar = lua_absindex(L, -1);
        i3ext_check_keys(L, titlebar, "i3ext.titlebar",
                         i3ext_titlebar_keys, I3EXT_ARRAY_LEN(i3ext_titlebar_keys));
        static const char *const aligns[] = {"left", "center", "right"};
        static const char *const visibilities[] = {"normal", "none"};
        long value;
        int ignored;
        i3ext_get_enum(L, titlebar, "align", "i3ext.titlebar.align",
                       aligns, I3EXT_ARRAY_LEN(aligns), &ignored);
        i3ext_get_int(L, titlebar, "height", "i3ext.titlebar.height", 0, 200, &value);
        i3ext_get_int(L, titlebar, "padding", "i3ext.titlebar.padding", 0, 100, &value);
        const char *font;
        i3ext_get_string(L, titlebar, "font", "i3ext.titlebar.font", &font);
        i3ext_get_enum(L, titlebar, "visibility", "i3ext.titlebar.visibility",
                       visibilities, I3EXT_ARRAY_LEN(visibilities), &ignored);
        lua_pop(L, 1);
    }

    if (i3ext_get_table(L, table, "client", "i3ext.client")) {
        i3ext_validate_client(L, -1, "i3ext.client");
        lua_pop(L, 1);
    }

    if (i3ext_get_table(L, table, "bar", "i3ext.bar")) {
        i3ext_validate_bar(L, -1, "i3ext.bar");
        lua_pop(L, 1);
    }

    if (i3ext_get_table(L, table, "switcher", "i3ext.switcher")) {
        i3ext_validate_switcher(L, -1, "i3ext.switcher");
        lua_pop(L, 1);
    }

    if (i3ext_get_table(L, table, "themes", "i3ext.themes")) {
        i3ext_validate_themes(L, -1, "i3ext.themes");
        lua_pop(L, 1);
    }

    i3ext_validate_theme_selection(L, table);

    if (i3ext_get_table(L, table, "animations", "i3ext.animations")) {
        const int animations = lua_absindex(L, -1);
        i3ext_check_keys(L, animations, "i3ext.animations",
                         i3ext_animations_keys, I3EXT_ARRAY_LEN(i3ext_animations_keys));

        bool enabled;
        i3ext_get_bool(L, animations, "enabled", "i3ext.animations.enabled", &enabled);

        if (i3ext_get_table(L, animations, "beziers", "i3ext.animations.beziers")) {
            const int beziers = lua_absindex(L, -1);
            lua_pushnil(L);
            while (lua_next(L, beziers)) {
                if (lua_type(L, -2) != LUA_TSTRING) {
                    luaL_error(L, "i3ext.animations.beziers: curve names must be strings");
                }
                const char *name = lua_tostring(L, -2);
                if (*name == '\0' || strchr(name, ',') != NULL ||
                    strpbrk(name, " \t\r\n") != NULL) {
                    luaL_error(L, "i3ext.animations.beziers: invalid curve name \"%s\"", name);
                }
                if (!lua_istable(L, -1)) {
                    luaL_error(L, "i3ext.animations.beziers.%s: expected table", name);
                }
                if (lua_rawlen(L, -1) != 4) {
                    luaL_error(L, "i3ext.animations.beziers.%s: expected 4 numbers", name);
                }
                for (int i = 1; i <= 4; i++) {
                    lua_geti(L, -1, i);
                    if (!lua_isnumber(L, -1)) {
                        luaL_error(L, "i3ext.animations.beziers.%s[%d]: expected number", name, i);
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
        }

        static const char *const anim_types[] = {
            "windows", "open", "close", "workspaces", "dropdown"};
        for (size_t i = 0; i < I3EXT_ARRAY_LEN(anim_types); i++) {
            char path[256];
            snprintf(path, sizeof(path), "i3ext.animations.%s", anim_types[i]);
            if (i3ext_get_table(L, animations, anim_types[i], path)) {
                i3ext_validate_animation_type(L, -1, path);
                lua_pop(L, 1);
            }
        }

        i3ext_get_bool(L, animations, "fade_in", "i3ext.animations.fade_in", &enabled);
        lua_pop(L, 1);
    }

    if (i3ext_get_table(L, table, "dropdown", "i3ext.dropdown")) {
        const int dropdown = lua_absindex(L, -1);
        i3ext_check_keys(L, dropdown, "i3ext.dropdown",
                         i3ext_dropdown_keys, I3EXT_ARRAY_LEN(i3ext_dropdown_keys));
        static const char *const positions[] = {"top", "bottom"};
        double height;
        int position;
        i3ext_get_number(L, dropdown, "height", "i3ext.dropdown.height", 0.1, 1.0, &height);
        i3ext_get_enum(L, dropdown, "position", "i3ext.dropdown.position",
                       positions, I3EXT_ARRAY_LEN(positions), &position);
        lua_pop(L, 1);
    }

    if (i3ext_get_table(L, table, "layouts", "i3ext.layouts")) {
        const int layouts = lua_absindex(L, -1);
        i3ext_check_keys(L, layouts, "i3ext.layouts",
                         i3ext_layouts_keys, I3EXT_ARRAY_LEN(i3ext_layouts_keys));

        if (i3ext_get_table(L, layouts, "master", "i3ext.layouts.master")) {
            const int master = lua_absindex(L, -1);
            i3ext_check_keys(L, master, "i3ext.layouts.master",
                             i3ext_master_keys, I3EXT_ARRAY_LEN(i3ext_master_keys));
            static const char *const orientations[] = {"left", "right", "top", "bottom"};
            double ratio;
            int orientation;
            i3ext_get_number(L, master, "ratio", "i3ext.layouts.master.ratio", 0.1, 0.9, &ratio);
            i3ext_get_enum(L, master, "orientation", "i3ext.layouts.master.orientation",
                           orientations, I3EXT_ARRAY_LEN(orientations), &orientation);
            lua_pop(L, 1);
        }

        if (i3ext_get_table(L, layouts, "dwindle", "i3ext.layouts.dwindle")) {
            const int dwindle = lua_absindex(L, -1);
            i3ext_check_keys(L, dwindle, "i3ext.layouts.dwindle",
                             i3ext_dwindle_keys, I3EXT_ARRAY_LEN(i3ext_dwindle_keys));
            double ratio;
            i3ext_get_number(L, dwindle, "split_ratio",
                             "i3ext.layouts.dwindle.split_ratio", 0.1, 0.9, &ratio);
            lua_pop(L, 1);
        }

        if (i3ext_get_table(L, layouts, "scroller", "i3ext.layouts.scroller")) {
            const int scroller = lua_absindex(L, -1);
            i3ext_check_keys(L, scroller, "i3ext.layouts.scroller",
                             i3ext_scroller_keys, I3EXT_ARRAY_LEN(i3ext_scroller_keys));
            double proportion;
            long structs;
            bool flag;
            i3ext_get_number(L, scroller, "proportion",
                             "i3ext.layouts.scroller.proportion", 0.1, 1.0, &proportion);
            i3ext_get_int(L, scroller, "structs",
                          "i3ext.layouts.scroller.structs", 0, 1000, &structs);
            i3ext_get_bool(L, scroller, "focus_center",
                           "i3ext.layouts.scroller.focus_center", &flag);
            i3ext_get_bool(L, scroller, "prefer_center",
                           "i3ext.layouts.scroller.prefer_center", &flag);
            i3ext_get_bool(L, scroller, "prefer_overspread",
                           "i3ext.layouts.scroller.prefer_overspread", &flag);
            i3ext_get_bool(L, scroller, "ignore_proportion_single",
                           "i3ext.layouts.scroller.ignore_proportion_single", &flag);
            i3ext_get_number(L, scroller, "default_proportion_single",
                             "i3ext.layouts.scroller.default_proportion_single", 0.1, 1.0,
                             &proportion);
            i3ext_get_bool(L, scroller, "autofit",
                           "i3ext.layouts.scroller.autofit", &flag);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}

static void i3ext_apply_color_list(lua_State *L, int idx, const char *key,
                                   const char *path, const char *classname,
                                   struct ConfigResultIR *result) {
    char colors[5][32];
    memset(colors, 0, sizeof(colors));
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const int list = lua_absindex(L, -1);
    for (int i = 1; i <= 5; i++) {
        lua_geti(L, list, i);
        const char *value = lua_tostring(L, -1);
        snprintf(colors[i - 1], sizeof(colors[i - 1]), "%s", value);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    (void)path;
    cfg_color(NULL, result, classname, colors[0], colors[1], colors[2], colors[3], colors[4]);
}

static void i3ext_apply_gradient(lua_State *L, int idx, const char *key,
                                 const char *path, const char *classname,
                                 struct ConfigResultIR *result) {
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const int gradient = lua_absindex(L, -1);
    const size_t len = lua_rawlen(L, gradient);
    char start[32], end[32];
    long angle = 45;
    lua_geti(L, gradient, 1);
    snprintf(start, sizeof(start), "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_geti(L, gradient, 2);
    snprintf(end, sizeof(end), "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
    if (len >= 3) {
        lua_geti(L, gradient, 3);
        angle = (long)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    char spec[128];
    snprintf(spec, sizeof(spec), "%s %s %ld", start, end, angle);
    (void)path;
    if (strcmp(key, "focused") == 0) {
        cfg_client_gradient_focused(NULL, result, spec);
    } else if (strcmp(key, "focused_inactive") == 0) {
        cfg_client_gradient_focused_inactive(NULL, result, spec);
    } else if (strcmp(key, "unfocused") == 0) {
        cfg_client_gradient_unfocused(NULL, result, spec);
    } else if (strcmp(key, "urgent") == 0) {
        cfg_client_gradient_urgent(NULL, result, spec);
    }
    (void)classname;
}

static void i3ext_apply_animation_type(lua_State *L, int animations, const char *key,
                                       const char *kind, int current_duration,
                                       struct ConfigResultIR *result) {
    char path[256];
    snprintf(path, sizeof(path), "i3ext.animations.%s", key);
    if (!i3ext_get_table(L, animations, key, path)) {
        return;
    }
    const int type = lua_absindex(L, -1);

    bool enabled = true;
    char field_path[280];
    snprintf(field_path, sizeof(field_path), "%s.enabled", path);
    i3ext_get_bool(L, type, "enabled", field_path, &enabled);

    long duration = current_duration;
    snprintf(field_path, sizeof(field_path), "%s.duration", path);
    i3ext_get_int(L, type, "duration", field_path, 0, 600000, &duration);

    const char *curve = NULL;
    snprintf(field_path, sizeof(field_path), "%s.curve", path);
    i3ext_get_string(L, type, "curve", field_path, &curve);

    char spec[512];
    const long effective = enabled ? duration : 0;
    if (curve != NULL) {
        snprintf(spec, sizeof(spec), "%s, %d, %ld, %s", kind, enabled ? 1 : 0,
                 effective, curve);
    } else {
        snprintf(spec, sizeof(spec), "%s, %d, %ld", kind, enabled ? 1 : 0, effective);
    }
    lua_pop(L, 1);
    cfg_animation(NULL, result, spec);
}

static void i3ext_apply_client(lua_State *L, int idx, const char *path,
                               struct ConfigResultIR *result) {
    idx = lua_absindex(L, idx);
    char sub[384];

    const char *background;
    snprintf(sub, sizeof(sub), "%s.background", path);
    if (i3ext_get_string(L, idx, "background", sub, &background)) {
        cfg_color_single(NULL, result, "client.background", background);
    }

    snprintf(sub, sizeof(sub), "%s.colors", path);
    if (i3ext_get_table(L, idx, "colors", sub)) {
        const int colors = lua_absindex(L, -1);
        static const char *const states[] = {
            "focused", "focused_inactive", "unfocused", "urgent"};
        for (size_t i = 0; i < I3EXT_ARRAY_LEN(states); i++) {
            char classname[64];
            snprintf(classname, sizeof(classname), "client.%s", states[i]);
            i3ext_apply_color_list(L, colors, states[i], sub, classname, result);
        }
        lua_pop(L, 1);
    }

    snprintf(sub, sizeof(sub), "%s.gradients", path);
    if (i3ext_get_table(L, idx, "gradients", sub)) {
        const int gradients = lua_absindex(L, -1);
        static const char *const states[] = {
            "focused", "focused_inactive", "unfocused", "urgent"};
        for (size_t i = 0; i < I3EXT_ARRAY_LEN(states); i++) {
            char classname[64];
            snprintf(classname, sizeof(classname), "client.%s", states[i]);
            i3ext_apply_gradient(L, gradients, states[i], sub, classname, result);
        }
        lua_pop(L, 1);
    }
}

static void i3ext_bar_set_single(const char *name, const char *value) {
    Barconfig *barconfig;
    TAILQ_FOREACH (barconfig, &barconfigs, configs) {
        char **slot = NULL;
        if (strcmp(name, "background") == 0) {
            slot = &barconfig->colors.background;
        } else if (strcmp(name, "statusline") == 0) {
            slot = &barconfig->colors.statusline;
        } else if (strcmp(name, "separator") == 0) {
            slot = &barconfig->colors.separator;
        } else if (strcmp(name, "focused_background") == 0) {
            slot = &barconfig->colors.focused_background;
        } else if (strcmp(name, "focused_statusline") == 0) {
            slot = &barconfig->colors.focused_statusline;
        } else if (strcmp(name, "focused_separator") == 0) {
            slot = &barconfig->colors.focused_separator;
        }
        if (slot == NULL) {
            continue;
        }
        FREE(*slot);
        *slot = sstrdup(value);
    }
}

static void i3ext_bar_set_triple(const char *name, const char *border,
                                 const char *background, const char *text) {
    Barconfig *barconfig;
    TAILQ_FOREACH (barconfig, &barconfigs, configs) {
        char **border_slot = NULL;
        char **bg_slot = NULL;
        char **text_slot = NULL;
        if (strcmp(name, "focused_workspace") == 0) {
            border_slot = &barconfig->colors.focused_workspace_border;
            bg_slot = &barconfig->colors.focused_workspace_bg;
            text_slot = &barconfig->colors.focused_workspace_text;
        } else if (strcmp(name, "active_workspace") == 0) {
            border_slot = &barconfig->colors.active_workspace_border;
            bg_slot = &barconfig->colors.active_workspace_bg;
            text_slot = &barconfig->colors.active_workspace_text;
        } else if (strcmp(name, "inactive_workspace") == 0) {
            border_slot = &barconfig->colors.inactive_workspace_border;
            bg_slot = &barconfig->colors.inactive_workspace_bg;
            text_slot = &barconfig->colors.inactive_workspace_text;
        } else if (strcmp(name, "urgent_workspace") == 0) {
            border_slot = &barconfig->colors.urgent_workspace_border;
            bg_slot = &barconfig->colors.urgent_workspace_bg;
            text_slot = &barconfig->colors.urgent_workspace_text;
        } else if (strcmp(name, "binding_mode") == 0) {
            border_slot = &barconfig->colors.binding_mode_border;
            bg_slot = &barconfig->colors.binding_mode_bg;
            text_slot = &barconfig->colors.binding_mode_text;
        }
        if (border_slot == NULL) {
            continue;
        }
        FREE(*border_slot);
        FREE(*bg_slot);
        FREE(*text_slot);
        *border_slot = sstrdup(border);
        *bg_slot = sstrdup(background);
        *text_slot = sstrdup(text);
    }
}

static void i3ext_apply_bar(lua_State *L, int idx, const char *path,
                            struct ConfigResultIR *result) {
    idx = lua_absindex(L, idx);
    (void)result;

    if (TAILQ_EMPTY(&barconfigs)) {
        luaL_error(L, "%s: no bar configuration found (bar {} must precede i3ext)", path);
    }

    for (size_t i = 0; i < I3EXT_ARRAY_LEN(i3ext_bar_single_keys); i++) {
        lua_getfield(L, idx, i3ext_bar_single_keys[i]);
        if (!lua_isnil(L, -1)) {
            i3ext_bar_set_single(i3ext_bar_single_keys[i], lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }

    for (size_t i = 0; i < I3EXT_ARRAY_LEN(i3ext_bar_triple_keys); i++) {
        lua_getfield(L, idx, i3ext_bar_triple_keys[i]);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        const int triple = lua_absindex(L, -1);
        lua_geti(L, triple, 1);
        const char *border = lua_tostring(L, -1);
        lua_geti(L, triple, 2);
        const char *background = lua_tostring(L, -1);
        lua_geti(L, triple, 3);
        const char *text = lua_tostring(L, -1);
        i3ext_bar_set_triple(i3ext_bar_triple_keys[i], border, background, text);
        lua_pop(L, 4);
    }
}

/* Applies a switcher theme table: { background, bar = { bg, text }, tile = { bg, text } }. */
static void i3ext_apply_switcher(lua_State *L, int idx, const char *path,
                                 struct ConfigResultIR *result) {
    idx = lua_absindex(L, idx);
    char sub[384];

    const char *background;
    snprintf(sub, sizeof(sub), "%s.background", path);
    if (i3ext_get_string(L, idx, "background", sub, &background)) {
        cfg_switcher_background(NULL, result, background);
    }

    snprintf(sub, sizeof(sub), "%s.bar", path);
    if (i3ext_get_table(L, idx, "bar", sub)) {
        char bg[32], text[32];
        lua_geti(L, -1, 1);
        snprintf(bg, sizeof(bg), "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_geti(L, -1, 2);
        snprintf(text, sizeof(text), "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pop(L, 1);
        cfg_switcher_bar(NULL, result, bg, text);
    }

    snprintf(sub, sizeof(sub), "%s.tile", path);
    if (i3ext_get_table(L, idx, "tile", sub)) {
        char bg[32], text[32];
        lua_geti(L, -1, 1);
        snprintf(bg, sizeof(bg), "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_geti(L, -1, 2);
        snprintf(text, sizeof(text), "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pop(L, 1);
        cfg_switcher_tile(NULL, result, bg, text);
    }
}

static bool i3ext_push_selected_theme(lua_State *L, int table) {
    table = lua_absindex(L, table);
    lua_getfield(L, table, "themes");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    const int themes = lua_absindex(L, -1);

    const char *selected = I3EXT_DEFAULT_THEME;
    lua_getfield(L, table, "theme");
    if (!lua_isnil(L, -1)) {
        selected = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, themes, selected);
    lua_remove(L, themes);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

static void i3ext_apply(lua_State *L, int table) {
    struct ConfigResultIR result = {0};
    table = lua_absindex(L, table);
    char value[64];

    /* Fork-wide switches live inside i3ext, not as native i3 directives. */
    bool enabled;
    if (i3ext_get_bool(L, table, "hot_reload", "i3ext.hot_reload", &enabled)) {
        cfg_hot_reload(NULL, &result, enabled ? "yes" : "no");
    }
    if (i3ext_get_bool(L, table, "status_line", "i3ext.status_line", &enabled)) {
        cfg_status_line(NULL, &result, enabled ? "yes" : "no");
    }
    if (i3ext_get_bool(L, table, "key_hints", "i3ext.key_hints", &enabled)) {
        config.key_hints = enabled;
        status_line_update();
    }

    /* 0. fork extension keybindings */
    if (i3ext_get_table(L, table, "binds", "i3ext.binds")) {
        const int binds = lua_absindex(L, -1);
        const size_t count = lua_rawlen(L, binds);
        for (size_t i = 1; i <= count; i++) {
            lua_geti(L, binds, (lua_Integer)i);
            const int entry = lua_absindex(L, -1);

            const char *full_key;
            const char *command;
            bool release = false;
            int type = 0;
            i3ext_get_string(L, entry, "key", "i3ext.binds[].key", &full_key);
            i3ext_get_string(L, entry, "command", "i3ext.binds[].command", &command);
            i3ext_get_bool(L, entry, "release", "i3ext.binds[].release", &release);
            i3ext_get_enum(L, entry, "type", "i3ext.binds[].type",
                           i3ext_bind_types, I3EXT_ARRAY_LEN(i3ext_bind_types), &type);

            /* Split "Mod4+Shift+t" at the last '+' into modifiers and key. */
            char *copy = sstrdup(full_key);
            char *plus = strrchr(copy, '+');
            const char *modifiers = "";
            const char *input = copy;
            if (plus != NULL) {
                *plus = '\0';
                modifiers = copy;
                input = plus + 1;
            }

            Binding *binding = configure_binding(i3ext_bind_types[type], modifiers,
                                                 input, release ? "release" : NULL,
                                                 NULL, NULL, NULL, command,
                                                 DEFAULT_BINDING_MODE, false);
            free(copy);
            lua_pop(L, 1);
            if (binding == NULL) {
                luaL_error(L, "i3ext.binds[%d]: could not register binding", (int)i);
            }
        }
        lua_pop(L, 1);
    }

    /* 0.5 selected theme: client first, then bar and switcher. */
    if (i3ext_push_selected_theme(L, table)) {
        const int theme = lua_absindex(L, -1);
        if (i3ext_get_table(L, theme, "client", "i3ext.theme.client")) {
            i3ext_apply_client(L, -1, "i3ext.theme.client", &result);
            lua_pop(L, 1);
        }
        if (i3ext_get_table(L, theme, "bar", "i3ext.theme.bar")) {
            i3ext_apply_bar(L, -1, "i3ext.theme.bar", &result);
            lua_pop(L, 1);
        }
        if (i3ext_get_table(L, theme, "switcher", "i3ext.theme.switcher")) {
            i3ext_apply_switcher(L, -1, "i3ext.theme.switcher", &result);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }


    if (i3ext_get_table(L, table, "gaps", "i3ext.gaps")) {
        const int gaps = lua_absindex(L, -1);
        for (size_t i = 0; i < I3EXT_ARRAY_LEN(i3ext_gap_keys); i++) {
            long pixels;
            char path[128];
            snprintf(path, sizeof(path), "i3ext.gaps.%s", i3ext_gap_keys[i]);
            if (i3ext_get_int(L, gaps, i3ext_gap_keys[i], path, 0, 1000, &pixels)) {
                cfg_gaps(NULL, &result, NULL, i3ext_gap_keys[i], pixels);
            }
        }
        lua_pop(L, 1);
    }

    /* 2. decoration */
    if (i3ext_get_table(L, table, "decoration", "i3ext.decoration")) {
        const int decoration = lua_absindex(L, -1);
        long number;
        if (i3ext_get_int(L, decoration, "rounding",
                          "i3ext.decoration.rounding", 0, 100, &number)) {
            snprintf(value, sizeof(value), "%ld", number);
            cfg_rounding(NULL, &result, value);
        }
        if (i3ext_get_int(L, decoration, "border_size",
                          "i3ext.decoration.border_size", 0, 100, &number)) {
            snprintf(value, sizeof(value), "%ld", number);
            cfg_border_size(NULL, &result, value);
        }
        lua_pop(L, 1);
    }

    /* 3. opacity */
    if (i3ext_get_table(L, table, "opacity", "i3ext.opacity")) {
        const int opacity = lua_absindex(L, -1);
        double number;
        if (i3ext_get_number(L, opacity, "active",
                             "i3ext.opacity.active", 0.0, 1.0, &number)) {
            snprintf(value, sizeof(value), "%g", number);
            cfg_active_opacity(NULL, &result, value);
        }
        if (i3ext_get_number(L, opacity, "inactive",
                             "i3ext.opacity.inactive", 0.0, 1.0, &number)) {
            snprintf(value, sizeof(value), "%g", number);
            cfg_inactive_opacity(NULL, &result, value);
        }
        bool flag;
        if (i3ext_get_bool(L, opacity, "dim_inactive",
                           "i3ext.opacity.dim_inactive", &flag)) {
            cfg_dim_inactive(NULL, &result, flag ? "yes" : "no");
        }
        if (i3ext_get_number(L, opacity, "dim_strength",
                             "i3ext.opacity.dim_strength", 0.0, 1.0, &number)) {
            snprintf(value, sizeof(value), "%g", number);
            cfg_dim_strength(NULL, &result, value);
        }
        lua_pop(L, 1);
    }

    /* 4. titlebar */
    if (i3ext_get_table(L, table, "titlebar", "i3ext.titlebar")) {
        const int titlebar = lua_absindex(L, -1);
        static const char *const aligns[] = {"left", "center", "right"};
        static const char *const visibilities[] = {"normal", "none"};
        int index;
        long number;
        if (i3ext_get_enum(L, titlebar, "align", "i3ext.titlebar.align",
                           aligns, I3EXT_ARRAY_LEN(aligns), &index)) {
            cfg_title_align(NULL, &result, aligns[index]);
        }
        if (i3ext_get_int(L, titlebar, "height",
                          "i3ext.titlebar.height", 0, 200, &number)) {
            snprintf(value, sizeof(value), "%ld", number);
            cfg_titlebar_height(NULL, &result, value);
        }
        if (i3ext_get_int(L, titlebar, "padding",
                          "i3ext.titlebar.padding", 0, 100, &number)) {
            snprintf(value, sizeof(value), "%ld", number);
            cfg_titlebar_padding(NULL, &result, value);
        }
        const char *font;
        if (i3ext_get_string(L, titlebar, "font", "i3ext.titlebar.font", &font)) {
            cfg_titlebar_font(NULL, &result, font);
        }
        if (i3ext_get_enum(L, titlebar, "visibility", "i3ext.titlebar.visibility",
                           visibilities, I3EXT_ARRAY_LEN(visibilities), &index)) {
            cfg_titlebar_visibility(NULL, &result, visibilities[index]);
        }
        lua_pop(L, 1);
    }

    /* 5. top-level client (overrides the theme) */
    if (i3ext_get_table(L, table, "client", "i3ext.client")) {
        i3ext_apply_client(L, -1, "i3ext.client", &result);
        lua_pop(L, 1);
    }

    /* 5.5 top-level bar (overrides the theme) */
    if (i3ext_get_table(L, table, "bar", "i3ext.bar")) {
        i3ext_apply_bar(L, -1, "i3ext.bar", &result);
        lua_pop(L, 1);
    }

    /* 5.6 top-level switcher (overrides the theme) */
    if (i3ext_get_table(L, table, "switcher", "i3ext.switcher")) {
        i3ext_apply_switcher(L, -1, "i3ext.switcher", &result);
        lua_pop(L, 1);
    }

    /* 8. animations: enabled, beziers, then the individual types. */
    if (i3ext_get_table(L, table, "animations", "i3ext.animations")) {
        const int animations = lua_absindex(L, -1);
        bool enabled;
        if (i3ext_get_bool(L, animations, "enabled",
                           "i3ext.animations.enabled", &enabled)) {
            cfg_animations(NULL, &result, enabled ? "on" : "off");
        }

        if (i3ext_get_table(L, animations, "beziers", "i3ext.animations.beziers")) {
            const int beziers = lua_absindex(L, -1);
            lua_pushnil(L);
            while (lua_next(L, beziers)) {
                const char *name = lua_tostring(L, -2);
                double points[4] = {0, 0, 0, 0};
                for (int i = 0; i < 4; i++) {
                    lua_geti(L, -1, i + 1);
                    points[i] = lua_tonumber(L, -1);
                    lua_pop(L, 1);
                }
                char spec[256];
                snprintf(spec, sizeof(spec), "%s, %.17g, %.17g, %.17g, %.17g",
                         name, points[0], points[1], points[2], points[3]);
                cfg_bezier(NULL, &result, spec);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }

        i3ext_apply_animation_type(L, animations, "windows", "windows",
                                   config.animation_duration, &result);
        i3ext_apply_animation_type(L, animations, "open", "windowsIn",
                                   config.animation_open_duration, &result);
        i3ext_apply_animation_type(L, animations, "close", "windowsOut",
                                   config.animation_close_duration, &result);
        i3ext_apply_animation_type(L, animations, "workspaces", "workspaces",
                                   config.animation_workspace_duration, &result);
        i3ext_apply_animation_type(L, animations, "dropdown", "dropdown",
                                   config.animation_dropdown_duration, &result);

        bool fade;
        if (i3ext_get_bool(L, animations, "fade_in",
                           "i3ext.animations.fade_in", &fade)) {
            cfg_animation(NULL, &result, fade ? "fadeIn, 1" : "fadeIn, 0");
        }
        lua_pop(L, 1);
    }

    /* 9. dropdown */
    if (i3ext_get_table(L, table, "dropdown", "i3ext.dropdown")) {
        const int dropdown = lua_absindex(L, -1);
        static const char *const positions[] = {"top", "bottom"};
        double height;
        int index;
        if (i3ext_get_number(L, dropdown, "height",
                             "i3ext.dropdown.height", 0.1, 1.0, &height)) {
            snprintf(value, sizeof(value), "%.17g", height);
            cfg_dropdown_height(NULL, &result, value);
        }
        if (i3ext_get_enum(L, dropdown, "position", "i3ext.dropdown.position",
                           positions, I3EXT_ARRAY_LEN(positions), &index)) {
            cfg_dropdown_position(NULL, &result, positions[index]);
        }
        lua_pop(L, 1);
    }

    /* 10. layouts */
    if (i3ext_get_table(L, table, "layouts", "i3ext.layouts")) {
        const int layouts = lua_absindex(L, -1);

        if (i3ext_get_table(L, layouts, "master", "i3ext.layouts.master")) {
            const int master = lua_absindex(L, -1);
            static const char *const orientations[] = {"left", "right", "top", "bottom"};
            int index;
            if (i3ext_get_enum(L, master, "orientation",
                               "i3ext.layouts.master.orientation",
                               orientations, I3EXT_ARRAY_LEN(orientations), &index)) {
                config.master_orientation = index;
            }
            double ratio;
            if (i3ext_get_number(L, master, "ratio",
                                 "i3ext.layouts.master.ratio", 0.1, 0.9, &ratio)) {
                snprintf(value, sizeof(value), "%.17g", ratio);
                cfg_master_ratio(NULL, &result, value);
            }
            lua_pop(L, 1);
        }

        if (i3ext_get_table(L, layouts, "dwindle", "i3ext.layouts.dwindle")) {
            const int dwindle = lua_absindex(L, -1);
            double ratio;
            if (i3ext_get_number(L, dwindle, "split_ratio",
                                 "i3ext.layouts.dwindle.split_ratio", 0.1, 0.9, &ratio)) {
                snprintf(value, sizeof(value), "%.17g", ratio);
                cfg_dwindle_split_ratio(NULL, &result, value);
            }
            lua_pop(L, 1);
        }

        if (i3ext_get_table(L, layouts, "scroller", "i3ext.layouts.scroller")) {
            const int scroller = lua_absindex(L, -1);
            double number;
            long integer;
            bool flag;
            if (i3ext_get_number(L, scroller, "proportion",
                                 "i3ext.layouts.scroller.proportion", 0.1, 1.0, &number)) {
                snprintf(value, sizeof(value), "%.17g", number);
                cfg_scroller_proportion(NULL, &result, value);
            }
            if (i3ext_get_int(L, scroller, "structs",
                              "i3ext.layouts.scroller.structs", 0, 1000, &integer)) {
                snprintf(value, sizeof(value), "%ld", integer);
                cfg_scroller_structs(NULL, &result, value);
            }
            if (i3ext_get_bool(L, scroller, "focus_center",
                               "i3ext.layouts.scroller.focus_center", &flag)) {
                cfg_scroller_focus_center(NULL, &result, flag ? "yes" : "no");
            }
            if (i3ext_get_bool(L, scroller, "prefer_center",
                               "i3ext.layouts.scroller.prefer_center", &flag)) {
                cfg_scroller_prefer_center(NULL, &result, flag ? "yes" : "no");
            }
            if (i3ext_get_bool(L, scroller, "prefer_overspread",
                               "i3ext.layouts.scroller.prefer_overspread", &flag)) {
                cfg_scroller_prefer_overspread(NULL, &result, flag ? "yes" : "no");
            }
            if (i3ext_get_bool(L, scroller, "ignore_proportion_single",
                               "i3ext.layouts.scroller.ignore_proportion_single", &flag)) {
                cfg_scroller_proportion_single(NULL, &result, flag ? "yes" : "no");
            }
            if (i3ext_get_number(L, scroller, "default_proportion_single",
                                 "i3ext.layouts.scroller.default_proportion_single",
                                 0.1, 1.0, &number)) {
                snprintf(value, sizeof(value), "%.17g", number);
                cfg_scroller_default_proportion_single(NULL, &result, value);
            }
            if (i3ext_get_bool(L, scroller, "autofit",
                               "i3ext.layouts.scroller.autofit", &flag)) {
                cfg_scroller_autofit(NULL, &result, flag ? "yes" : "no");
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}

static int i3ext_lua_i3ext(lua_State *L) {
    if (!lua_istable(L, 1)) {
        luaL_error(L, "i3ext: expected a table");
    }
    const int table = lua_absindex(L, 1);
    /* Two phases: validate everything first so that no setter runs when the
     * block contains an unknown key or an invalid value, then apply the
     * setters in the fixed order. */
    i3ext_validate(L, table);
    i3ext_apply(L, table);
    return 0;
}

static int i3ext_traceback(lua_State *L) {
    const char *message = lua_tostring(L, 1);
    if (message != NULL) {
        luaL_traceback(L, L, message, 1);
    } else {
        lua_pushliteral(L, "(error object is not a string)");
    }
    return 1;
}

bool i3ext_run(long index) {
    struct i3ext_chunk *chunk = NULL;
    for (struct i3ext_chunk *c = i3ext_chunks; c != NULL; c = c->next) {
        if (c->id == index) {
            chunk = c;
            break;
        }
    }
    if (chunk == NULL) {
        ELOG("i3ext: unknown block id %ld\n", index);
        return false;
    }

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        ELOG("i3ext: could not create Lua state\n");
        return false;
    }

    /* Only base/table/string/math/utf8 are available in the sandbox. */
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);

    /* Build the sandbox environment as a copy of the (already restricted)
     * global table, then remove everything that could escape the sandbox. */
    lua_newtable(L);
    const int env = lua_absindex(L, -1);
    lua_pushglobaltable(L);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_rawset(L, env);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    static const char *const denied[] = {
        "os", "io", "package", "debug", "require", "load", "loadfile",
        "dofile", "loadstring", "collectgarbage", "print"};
    for (size_t i = 0; i < I3EXT_ARRAY_LEN(denied); i++) {
        lua_pushnil(L);
        lua_setfield(L, env, denied[i]);
    }
    lua_pushvalue(L, env);
    lua_setfield(L, env, "_G");
    lua_pushcfunction(L, i3ext_lua_i3ext);
    lua_setfield(L, env, "i3ext");

    /* Message handler for pcall. */
    lua_pushcfunction(L, i3ext_traceback);
    const int msgh = lua_gettop(L);

    char chunkname[PATH_MAX + 2];
    snprintf(chunkname, sizeof(chunkname), "@%s", chunk->filename);
    if (luaL_loadbufferx(L, chunk->source, strlen(chunk->source),
                         chunkname, "t") != LUA_OK) {
        const char *error = lua_tostring(L, -1);
        ELOG("i3ext: %s\n", error ? error : "(unknown Lua error)");
        lua_close(L);
        return false;
    }

    lua_pushvalue(L, env);
    if (lua_setupvalue(L, -2, 1) == NULL) {
        ELOG("i3ext: failed to set the sandbox environment\n");
        lua_close(L);
        return false;
    }

    if (lua_pcall(L, 0, 0, msgh) != LUA_OK) {
        const char *error = lua_tostring(L, -1);
        ELOG("i3ext: %s\n", error ? error : "(unknown Lua error)");
        lua_close(L);
        return false;
    }

    lua_close(L);
    return true;
}

#endif /* I3EXT_SCAN_TEST */
