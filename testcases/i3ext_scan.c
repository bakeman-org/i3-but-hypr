/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * testcases/i3ext_scan.c: unit tests for the Lua-aware i3ext block scanner.
 *
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "all.h"
#include "i3ext.h"

/* The scanner only needs ELOG; provide the minimal logging stubs so that we
 * do not have to link the whole window manager. */
void errorlog(char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

void verboselog(char *fmt, ...) {
}

void debuglog(char *fmt, ...) {
}

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static size_t scan(const char *config, I3ExtLineRange **ranges) {
    size_t count = 0;
    if (!i3ext_find_blocks(config, "<test>", ranges, &count)) {
        free(*ranges);
        *ranges = NULL;
        return (size_t)-1;
    }
    return count;
}

static void test_basic_block(void) {
    const char *config =
        "font pango:monospace 10\n"
        "i3ext {\n"
        "    gaps = { inner = 8 },\n"
        "}\n"
        "gaps_in 4\n";

    I3ExtLineRange *ranges = NULL;
    const size_t count = scan(config, &ranges);
    CHECK(count == 1);
    if (count == 1) {
        CHECK(ranges[0].start_line == 2);
        CHECK(ranges[0].end_line == 4);
    }
    free(ranges);
}

static void test_lua_lexing(void) {
    const char *config =
        "i3ext {\n"
        "    a = \"}\",\n"
        "    b = '}',\n"
        "    c = \"escaped \\\" } still string\",\n"
        "    -- } line comment\n"
        "    --[[ } long\n"
        "         comment ]]\n"
        "    d = [[ } long string ]],\n"
        "    e = [==[ } ]] with = signs ]==],\n"
        "    f = { nested = { 1, 2 } },\n"
        "}\n"
        "after\n";

    I3ExtLineRange *ranges = NULL;
    const size_t count = scan(config, &ranges);
    CHECK(count == 1);
    if (count == 1) {
        CHECK(ranges[0].start_line == 1);
        CHECK(ranges[0].end_line == 11);
    }
    free(ranges);
}

static void test_crlf(void) {
    const char *config =
        "i3ext {\r\n"
        "    a = 1,\r\n"
        "}\r\n"
        "x\r\n";

    I3ExtLineRange *ranges = NULL;
    const size_t count = scan(config, &ranges);
    CHECK(count == 1);
    if (count == 1) {
        CHECK(ranges[0].start_line == 1);
        CHECK(ranges[0].end_line == 3);
    }
    free(ranges);
}

static void test_multiple_blocks(void) {
    const char *config =
        "a\n"
        "i3ext {\n"
        "    x = 1,\n"
        "}\n"
        "b\n"
        "i3ext {\n"
        "    y = 2,\n"
        "    z = { 1 },\n"
        "}\n"
        "c\n";

    I3ExtLineRange *ranges = NULL;
    const size_t count = scan(config, &ranges);
    CHECK(count == 2);
    if (count == 2) {
        CHECK(ranges[0].start_line == 2);
        CHECK(ranges[0].end_line == 4);
        CHECK(ranges[1].start_line == 6);
        CHECK(ranges[1].end_line == 9);
    }
    free(ranges);
}

static void test_native_blocks_with_colors(void) {
    const char *config =
        "bar { colors { background #111111 } }\n"
        "mode \"resize\" {\n"
        "    bindsym j resize shrink width 10 px or 10 ppt\n"
        "}\n"
        "i3ext {\n"
        "    x = 1,\n"
        "}\n";

    I3ExtLineRange *ranges = NULL;
    const size_t count = scan(config, &ranges);
    CHECK(count == 1);
    if (count == 1) {
        CHECK(ranges[0].start_line == 5);
        CHECK(ranges[0].end_line == 7);
    }
    free(ranges);
}

static void test_invalid_blocks(void) {
    I3ExtLineRange *ranges = NULL;

    CHECK(scan("i3ext {\n  a = 1,\n", &ranges) == (size_t)-1);
    CHECK(ranges == NULL);
    CHECK(scan("i3ext { a = 1 }\n", &ranges) == (size_t)-1);
    CHECK(ranges == NULL);
    CHECK(scan("bar {\n  i3ext {\n    a = 1,\n  }\n}\n", &ranges) == (size_t)-1);
    CHECK(ranges == NULL);
    CHECK(scan("i3ext {\n  a = 1,\n} trailing\n", &ranges) == (size_t)-1);
    CHECK(ranges == NULL);
}

static size_t line_count(const char *text) {
    size_t count = 1;
    for (const char *walk = text; *walk != '\0'; walk++) {
        if (*walk == '\n') {
            count++;
        }
    }
    return count;
}

static void test_preprocess(void) {
    const char *config =
        "font x\n"
        "i3ext {\n"
        "    gaps = { inner = 8 },\n"
        "}\n"
        "gaps_in 4\n";

    i3ext_registry_reset();
    bool has_errors = false;
    char *processed = i3ext_preprocess(config, "<test>", &has_errors);
    CHECK(!has_errors);
    CHECK(processed != NULL);
    if (processed != NULL) {
        CHECK(strstr(processed, "__i3ext_exec 0") != NULL);
        CHECK(strstr(processed, "gaps_in 4") != NULL);
        CHECK(strstr(processed, "inner = 8") == NULL);
        CHECK(line_count(processed) == line_count(config));
    }
    CHECK(i3ext_registry_count() == 1);

    int start_line = 0;
    const char *source = i3ext_chunk_source(0, &start_line);
    CHECK(source != NULL);
    CHECK(start_line == 2);
    if (source != NULL) {
        /* One leading newline for line alignment, then the block itself. */
        CHECK(source[0] == '\n');
        CHECK(strstr(source, "i3ext {") != NULL);
    }

    free(processed);
    i3ext_registry_reset();
    CHECK(i3ext_registry_count() == 0);
}

int main(void) {
    test_basic_block();
    test_lua_lexing();
    test_crlf();
    test_multiple_blocks();
    test_native_blocks_with_colors();
    test_invalid_blocks();
    test_preprocess();

    if (failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("i3ext scanner tests passed\n");
    return 0;
}
