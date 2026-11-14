/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * config_diag.c: structured collection of configuration problems.
 *
 */
#include "all.h"

#include <ctype.h>

#include "config_diag.h"

#define CONFIG_DIAG_MAX 32

static struct config_diag diags[CONFIG_DIAG_MAX];
static int diag_count;

void config_diag_reset(void) {
    for (int i = 0; i < diag_count; i++) {
        FREE(diags[i].file);
        FREE(diags[i].message);
        FREE(diags[i].hint);
    }
    memset(diags, 0, sizeof(diags));
    diag_count = 0;
}

void config_diag_add(const char *file, int line, const char *message, const char *hint) {
    if (diag_count >= CONFIG_DIAG_MAX) {
        return;
    }
    struct config_diag *diag = &diags[diag_count++];
    diag->file = (file != NULL ? sstrdup(file) : NULL);
    diag->line = line;
    diag->message = sstrdup(message != NULL ? message : "(unknown error)");
    diag->hint = (hint != NULL ? sstrdup(hint) : NULL);
}

int config_diag_count(void) {
    return diag_count;
}

const struct config_diag *config_diag_get(int index) {
    if (index < 0 || index >= diag_count) {
        return NULL;
    }
    return &diags[index];
}

/* Returns the basename of a path (for compact nagbar messages). */
static const char *diag_basename(const char *path) {
    if (path == NULL) {
        return "<config>";
    }
    const char *slash = strrchr(path, '/');
    return (slash != NULL ? slash + 1 : path);
}

char *config_diag_summary(void) {
    if (diag_count == 0) {
        return NULL;
    }
    const struct config_diag *diag = &diags[0];
    char *message = sstrdup(diag->message);
    char *newline = strchr(message, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }
    /* Keep the nagbar from becoming extremely wide. */
    const size_t max_len = 110;
    if (strlen(message) > max_len) {
        message[max_len] = '\0';
        strlcat(message, "…", max_len + 8);
    }

    char location[PATH_MAX + 32];
    if (diag->line > 0) {
        snprintf(location, sizeof(location), "%s:%d", diag_basename(diag->file), diag->line);
    } else {
        snprintf(location, sizeof(location), "%s", diag_basename(diag->file));
    }

    char *result;
    sasprintf(&result, "%s: %s", location, message);
    free(message);
    return result;
}

/* Prints up to three lines of source context around `line`, marking the
 * offending line with '>'. */
static void diag_print_context(FILE *stream, const char *path, int line, bool color) {
    if (path == NULL || line <= 0) {
        return;
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return;
    }

    char buffer[1024];
    for (int current = 1; fgets(buffer, sizeof(buffer), file) != NULL; current++) {
        if (current < line - 1) {
            continue;
        }
        if (current > line + 1) {
            break;
        }
        /* Trim the trailing newline so that the caret line lines up. */
        const size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n') {
            buffer[len - 1] = '\0';
        }
        if (current == line) {
            fprintf(stream, "%s  > %4d | %s%s\n",
                    color ? "\033[1;31m" : "", current, buffer, color ? "\033[0m" : "");
        } else {
            fprintf(stream, "%s    %4d | %s%s\n",
                    color ? "\033[90m" : "", current, buffer, color ? "\033[0m" : "");
        }
    }
    fclose(file);
}

void config_diag_print(FILE *stream, bool color) {
    const char *red = color ? "\033[1;31m" : "";
    const char *yellow = color ? "\033[1;33m" : "";
    const char *bold = color ? "\033[1m" : "";
    const char *reset = color ? "\033[0m" : "";

    if (diag_count == 0) {
        fprintf(stream, "%si3: 配置检查通过%s\n", color ? "\033[1;32m" : "", reset);
        return;
    }

    /* Compact, greppable first line (also used by the config watcher). */
    char *summary = config_diag_summary();
    fprintf(stream, "i3: 配置错误: %s\n", summary);
    free(summary);

    fprintf(stream, "\n%s发现 %d 个配置错误%s（下面的行号与竖线指示具体位置）：\n\n",
            bold, diag_count, reset);

    for (int i = 0; i < diag_count; i++) {
        const struct config_diag *diag = &diags[i];
        if (diag->line > 0) {
            fprintf(stream, "%s[%d/%d]%s %s:%d\n",
                    yellow, i + 1, diag_count, reset,
                    diag->file != NULL ? diag->file : "<unknown>", diag->line);
        } else {
            fprintf(stream, "%s[%d/%d]%s %s\n",
                    yellow, i + 1, diag_count, reset,
                    diag->file != NULL ? diag->file : "<unknown>");
        }

        diag_print_context(stream, diag->file, diag->line, color);

        fprintf(stream, "      %s原因:%s %s\n", red, reset, diag->message);
        if (diag->hint != NULL) {
            fprintf(stream, "      %s建议:%s %s\n", bold, reset, diag->hint);
        }
        fprintf(stream, "\n");
    }

    fprintf(stream,
            "下一步：\n"
            "  1. 按上面的文件:行号修改配置（编辑器里跳转即可）。\n"
            "  2. 重新检查：i3 -C -c %s\n"
            "  3. 修好后保存文件（开启了 hot_reload 会自动重载），或执行 i3-msg reload。\n"
            "  4. 完整语法见 docs/config-spec.zh.md；fork 扩展见第 12 章 i3ext。\n",
            diags[0].file != NULL ? diags[0].file : "~/.config/i3/config");
}

const char *config_diag_hint_parse(const char *source_line, const char *message) {
    const char *line = (source_line != NULL ? source_line : "");
    const char *trimmed = line;
    while (*trimmed == ' ' || *trimmed == '\t') {
        trimmed++;
    }
    const bool indented = (trimmed != line);
    const size_t len = strlen(trimmed);

    /* Lua-style assignment: `key = value,` / `key = {` / booleans. This is by
     * far the most common mistake (fork extensions outside i3ext {}). */
    const bool looks_lua =
        (strchr(trimmed, '=') != NULL &&
         ((len > 0 && trimmed[len - 1] == ',') ||
          strstr(trimmed, "= {") != NULL ||
          strstr(trimmed, "= true") != NULL ||
          strstr(trimmed, "= false") != NULL));

    if (looks_lua) {
        return "这一行是 Lua 赋值写法。fork 扩展字段（float_size、status_line、hot_reload、"
               "theme、binds、gaps、layouts、animations、dropdown 等）必须写在顶层 "
               "`i3ext { ... }` 块内，块内字段之间用逗号、注释用 `--`；块外只能写上游 i3 "
               "指令。例如：\n"
               "          i3ext {\n"
               "              float_size = { width = 0.6, height = 0.6 },\n"
               "          }";
    }
    if (indented) {
        return "i3 指令必须从行首开始，不能缩进；缩进行会被当成新指令，从而报“无法识别”。"
               "把这一行顶格即可。";
    }
    if (message != NULL && strstr(message, "Expected one of these tokens") != NULL) {
        return "无法识别这一行的开头。常见原因：指令拼写错误、把 fork 扩展写在 i3ext 块外、"
               "或在 i3 配置里写了 Lua 语法。可用指令见 docs/config-spec.zh.md，"
               "fork 扩展见第 12 章。";
    }
    if (message != NULL && (strstr(message, "'}'") != NULL || strstr(message, "'{'") != NULL)) {
        return "花括号不匹配：`bar {`、`mode \"...\" {`、`i3ext {` 都必须用单独一行的 `}` 闭合。";
    }
    return "对照 docs/config-spec.zh.md 检查这一行；不确定时可以先在行首加上 # 注释掉，"
           "保存后会自动重新校验并给出新的提示。";
}

const char *config_diag_hint_i3ext(const char *message) {
    if (message == NULL) {
        return NULL;
    }
    if (strstr(message, "unknown field") != NULL) {
        return "字段名不在该表允许的键里。顶层可用键见 docs/config-spec.zh.md 12.2，"
               "各表字段见 12.3 字段总表；注意大小写与拼写。";
    }
    if (strstr(message, "out of range") != NULL) {
        return "数值超出允许范围：错误信息里方括号中的 [min, max] 就是允许区间。";
    }
    if (strstr(message, "expected number") != NULL) {
        return "这里需要数字（不要加引号），例如 width = 0.6。";
    }
    if (strstr(message, "expected boolean") != NULL) {
        return "这里需要 true 或 false（Lua 布尔值，不加引号）。";
    }
    if (strstr(message, "expected string") != NULL) {
        return "这里需要字符串，例如 theme = \"catppuccin_mocha\"。";
    }
    if (strstr(message, "expected table") != NULL) {
        return "这里需要表，例如 float_size = { width = 0.6, height = 0.6 }，"
               "或 binds = { { key = \"$mod+x\", command = \"kill\" } }。";
    }
    if (strstr(message, "hex color") != NULL) {
        return "颜色必须写成带引号的 \"#rrggbb\" 或 \"#rrggbbaa\"。";
    }
    if (strstr(message, "expected integer") != NULL) {
        return "这里需要整数（不能带小数点）。";
    }
    if (strstr(message, "syntax error") != NULL ||
        strstr(message, "unexpected symbol") != NULL ||
        strstr(message, "near") != NULL) {
        return "Lua 语法错误：字段之间用逗号，字符串用双引号，注释用 --，表用 { }。";
    }
    return "看错误里的字段路径（i3ext.xxx.yyy），对照 docs/config-spec.zh.md 第 12 章；"
           "改完保存文件即可热重载。";
}
