/* tool_search.c — glob and grep tools.
 * C ports of Claude Code's GlobTool (fast filename pattern matching, results
 * sorted by modification time, truncated at 100) and GrepTool (line-oriented
 * text search with files_with_matches / content / count output modes,
 * glob-filtered, head_limit capped). The pattern engine is a simplified
 * subset: glob supports *, ? and **; grep matches literal text (optionally
 * case-insensitive) instead of full ripgrep regex. */
#include "cagent/action/tools.h"
#include "cagent/os/os_fs.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "cJSON.h"

/* ---------- shared path helpers ---------- */

static void resolve_root(const ca_tool_ctx *ctx, const char *maybe_rel,
                         char *out, size_t n) {
    if (maybe_rel && *maybe_rel)
        ca_path_resolve(out, n, ctx ? ctx->workspace : NULL, maybe_rel);
    else if (ctx && ctx->workspace && *ctx->workspace)
        snprintf(out, n, "%s", ctx->workspace);
    else
        snprintf(out, n, ".");
}

static long long file_mtime(const char *path) {
#ifdef _WIN32
    struct _stat st;
    if (_stat(path, &st) != 0) return 0;
    return (long long)st.st_mtime;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_mtime;
#endif
}

static int ci_char(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ci_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (ci_char((unsigned char)a[i]) != ci_char((unsigned char)b[i])) return 1;
    return 0;
}

/* ---------- glob matching (subset of GlobTool / utils/glob) ----------
 * pattern segments are separated by '/'; "**" matches zero or more whole
 * segments; within a segment '*' matches any run and '?' one char. */

static int seg_match(const char *seg, size_t seglen, const char *s) {
    /* iterative wildcard match of one path segment against NUL-terminated s */
    const char *star = NULL, *ss = s, *star_s = NULL;
    const char *p = seg, *pe = seg + seglen;
    while (*ss) {
        if (p < pe && (*p == '?' || *p == *ss)) { p++; ss++; }
        else if (p < pe && *p == '*') { star = p++; star_s = ss; }
        else if (star) { p = star + 1; ss = ++star_s; }
        else return 0;
    }
    while (p < pe && *p == '*') p++;
    return p == pe;
}

static int glob_match_segs(char *pat_rest, char *path_rest) {
    /* split next segment of each side; recursive with ** expansion */
    while (*pat_rest) {
        char *pslash = strchr(pat_rest, '/');
        size_t plen = pslash ? (size_t)(pslash - pat_rest) : strlen(pat_rest);

        if (plen == 2 && pat_rest[0] == '*' && pat_rest[1] == '*') {
            /* consume consecutive ** segments at once */
            pat_rest = pslash ? pslash + 1 : pat_rest + plen;
            if (!*pat_rest) return 1; /* trailing ** matches everything left */
            /* try matching the remainder at every remaining position */
            while (1) {
                if (glob_match_segs(pat_rest, path_rest)) return 1;
                char *qslash = strchr(path_rest, '/');
                if (!qslash) return 0;
                path_rest = qslash + 1;
            }
        }
        char *aslash = strchr(path_rest, '/');
        size_t alen = aslash ? (size_t)(aslash - path_rest) : strlen(path_rest);
        /* no length pre-check here: wildcard segments (*, ?) legitimately
         * match segments of a different length (e.g. "*.cpp" vs "main.cpp") */
        char tmp[256];
        if (alen >= sizeof(tmp)) return 0;
        memcpy(tmp, path_rest, alen); tmp[alen] = '\0';
        if (!seg_match(pat_rest, plen, tmp)) return 0;
        if (!pslash) return aslash == NULL;
        if (!aslash) return 0;
        pat_rest = pslash + 1;
        path_rest = aslash + 1;
    }
    return *path_rest == '\0';
}

typedef struct {
    char *path;          /* relative path, '/'-separated */
    long long mtime;
} found_entry;

typedef struct {
    found_entry *items;
    size_t count, cap;
} found_list;

static void found_push(found_list *fl, const char *rel, long long mt) {
    if (fl->count == fl->cap) {
        size_t nc = fl->cap ? fl->cap * 2 : 32;
        found_entry *ni = (found_entry *)realloc(fl->items, nc * sizeof(found_entry));
        if (!ni) return;
        fl->items = ni;
        fl->cap = nc;
    }
    fl->items[fl->count].path = ca_strdup(rel);
    fl->items[fl->count].mtime = mt;
    fl->count++;
}

/* newest first (like GlobTool); ties broken by path for determinism */
static int cmp_mtime_desc(const void *a, const void *b) {
    const found_entry *ea = (const found_entry *)a, *eb = (const found_entry *)b;
    if (ea->mtime != eb->mtime) return ea->mtime > eb->mtime ? -1 : 1;
    return strcmp(ea->path, eb->path);
}

static void walk_dir(const char *root, const char *rel, const char *pattern,
                     found_list *fl) {
    char full[2048];
    ca_path_join(full, sizeof full, root, rel && *rel ? rel : "");
    ca_dir_list dl;
    memset(&dl, 0, sizeof dl);
    if (ca_fs_list_dir(full, &dl) != 0) return;
    for (size_t i = 0; i < dl.count; i++) {
        const char *name = dl.items[i].name;
        if (!name || !*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child[1024];
        if (rel && *rel) snprintf(child, sizeof child, "%s/%s", rel, name);
        else snprintf(child, sizeof child, "%s", name);
        if (dl.items[i].is_dir) {
            if (strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0)
                continue;
            walk_dir(root, child, pattern, fl);
        } else {
            if (glob_match_segs((char *)pattern, child)) {
                char child_full[2048];
                ca_path_join(child_full, sizeof child_full, root, child);
                found_push(fl, child, file_mtime(child_full));
            }
        }
    }
    ca_fs_list_free(&dl);
}

static ca_tool_result *glob_exec(const ca_tool *self, const ca_tool_ctx *ctx,
                                 const char *args_json) {
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return ca_tool_result_new(0, "glob: invalid args JSON");
    cJSON *pat_j = cJSON_GetObjectItemCaseSensitive(args, "pattern");
    if (!pat_j || !cJSON_IsString(pat_j)) {
        cJSON_Delete(args);
        return ca_tool_result_new(0, "glob: missing string arg 'pattern'");
    }
    cJSON *path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    char root[1024];
    resolve_root(ctx, (path_j && cJSON_IsString(path_j)) ? path_j->valuestring : NULL,
                 root, sizeof root);
    char *pat = ca_strdup(pat_j->valuestring);
    cJSON_Delete(args);

    /* normalize pattern separators to '/' */
    for (char *p = pat; *p; p++)
        if (*p == '\\') *p = '/';

    found_list fl;
    memset(&fl, 0, sizeof fl);
    walk_dir(root, NULL, pat, &fl);
    free(pat);

    /* sort by modification time (newest first), like GlobTool */
    if (fl.count > 1) qsort(fl.items, fl.count, sizeof(found_entry), cmp_mtime_desc);
    const size_t LIMIT = 100;
    int truncated = fl.count > LIMIT;
    ca_strbuf sb;
    ca_strbuf_init(&sb);
    if (fl.count == 0) ca_strbuf_append(&sb, "No files found");
    size_t shown = fl.count < LIMIT ? fl.count : LIMIT;
    for (size_t i = 0; i < shown; i++)
        ca_strbuf_appendf(&sb, "%s\n", fl.items[i].path);
    if (truncated)
        ca_strbuf_appendf(&sb,
                          "(Results are truncated from %zu. Consider using a more specific path or pattern.)\n",
                          fl.count);
    for (size_t i = 0; i < fl.count; i++) free(fl.items[i].path);
    free(fl.items);
    char *out = ca_strbuf_detach(&sb);
    ca_tool_result *r = ca_tool_result_new(1, out ? out : "");
    free(out);
    return r;
}

/* ---------- grep (subset of GrepTool) ---------- */

typedef struct {
    const char *needle;      /* text to find */
    size_t needle_len;
    const char *file_glob;   /* optional file-name glob filter, may be NULL */
    int ignore_case;
    int mode;                /* 0 files_with_matches, 1 content, 2 count */
    size_t head_limit;
    ca_strbuf sb;            /* output */
    size_t out_n;            /* entries emitted */
    int truncated;
} grep_state;

static int line_has_needle(const char *line, size_t len, const grep_state *g) {
    if (g->needle_len == 0 || len < g->needle_len) return 0;
    for (size_t i = 0; i + g->needle_len <= len; i++) {
        if (g->ignore_case) {
            if (ci_char((unsigned char)line[i]) == ci_char((unsigned char)g->needle[0]) &&
                ci_strncmp(line + i, g->needle, g->needle_len) == 0) return 1;
        } else if (line[i] == g->needle[0] &&
                   memcmp(line + i, g->needle, g->needle_len) == 0) return 1;
    }
    return 0;
}

static void grep_file(const char *rel, const char *full, grep_state *g) {
    char *text = ca_fs_read_file(full);
    if (!text) return;
    /* skip binary-looking files */
    if (memchr(text, '\0', strlen(text) < 8192 ? strlen(text) : 8192)) {
        free(text);
        return;
    }
    size_t hits = 0, lineno = 0;
    char *p = text;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; }
        else p += strlen(p);
        lineno++;
        if (line_has_needle(line, strlen(line), g)) {
            hits++;
            if (g->mode == 1) {
                if (g->out_n < g->head_limit) {
                    ca_strbuf_appendf(&g->sb, "%s:%zu:%s\n", rel, lineno, line);
                    g->out_n++;
                } else g->truncated = 1;
            }
        }
        if (!nl) break;
    }
    if (hits > 0) {
        if (g->mode == 0) {
            if (g->out_n < g->head_limit) { ca_strbuf_appendf(&g->sb, "%s\n", rel); g->out_n++; }
            else g->truncated = 1;
        } else if (g->mode == 2) {
            if (g->out_n < g->head_limit) { ca_strbuf_appendf(&g->sb, "%s:%zu\n", rel, hits); g->out_n++; }
            else g->truncated = 1;
        }
    }
    free(text);
}

static int name_glob_ok(const char *name, const char *glob_pat) {
    if (!glob_pat || !*glob_pat) return 1;
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", name);
    return glob_match_segs((char *)glob_pat, tmp);
}

static void grep_walk(const char *root, const char *rel, grep_state *g) {
    char full[2048];
    ca_path_join(full, sizeof full, root, rel && *rel ? rel : "");
    if (ca_fs_is_dir(full)) {
        ca_dir_list dl;
        memset(&dl, 0, sizeof dl);
        if (ca_fs_list_dir(full, &dl) != 0) return;
        for (size_t i = 0; i < dl.count; i++) {
            const char *name = dl.items[i].name;
            if (!name || !*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            if (strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0)
                continue;
            char child[1024];
            if (rel && *rel) snprintf(child, sizeof child, "%s/%s", rel, name);
            else snprintf(child, sizeof child, "%s", name);
            grep_walk(root, child, g);
        }
        ca_fs_list_free(&dl);
    } else {
        /* file: apply the optional glob name filter */
        const char *base = strrchr(rel, '/');
        base = base ? base + 1 : rel;
        if (!name_glob_ok(base, g->file_glob)) return;
        grep_file(rel, full, g);
    }
}

static ca_tool_result *grep_exec(const ca_tool *self, const ca_tool_ctx *ctx,
                                 const char *args_json) {
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return ca_tool_result_new(0, "grep: invalid args JSON");
    cJSON *pat_j = cJSON_GetObjectItemCaseSensitive(args, "pattern");
    if (!pat_j || !cJSON_IsString(pat_j)) {
        cJSON_Delete(args);
        return ca_tool_result_new(0, "grep: missing string arg 'pattern'");
    }
    cJSON *path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    cJSON *glob_j = cJSON_GetObjectItemCaseSensitive(args, "glob");
    cJSON *mode_j = cJSON_GetObjectItemCaseSensitive(args, "output_mode");
    cJSON *ic_j = cJSON_GetObjectItemCaseSensitive(args, "ignore_case");
    cJSON *hl_j = cJSON_GetObjectItemCaseSensitive(args, "head_limit");

    grep_state g;
    memset(&g, 0, sizeof g);
    /* copy strings out before cJSON_Delete(args) frees the tree */
    g.needle = ca_strdup(pat_j->valuestring);
    g.needle_len = strlen(g.needle);
    g.file_glob = (glob_j && cJSON_IsString(glob_j) && *glob_j->valuestring)
                      ? ca_strdup(glob_j->valuestring) : NULL;
    g.ignore_case = (ic_j && cJSON_IsTrue(ic_j)) ? 1 : 0;
    g.mode = 0;
    if (mode_j && cJSON_IsString(mode_j)) {
        if (strcmp(mode_j->valuestring, "content") == 0) g.mode = 1;
        else if (strcmp(mode_j->valuestring, "count") == 0) g.mode = 2;
    }
    g.head_limit = (hl_j && cJSON_IsNumber(hl_j) && hl_j->valuedouble > 0)
                       ? (size_t)hl_j->valuedouble : 250;

    char root[1024];
    resolve_root(ctx, (path_j && cJSON_IsString(path_j)) ? path_j->valuestring : NULL,
                 root, sizeof root);
    cJSON_Delete(args);

    ca_strbuf_init(&g.sb);
    grep_walk(root, NULL, &g);
    if (g.out_n == 0) ca_strbuf_append(&g.sb, "No matches found");
    if (g.truncated)
        ca_strbuf_appendf(&g.sb, "(Results are truncated. Consider using a more specific path, pattern or head_limit.)\n");
    free((void *)g.needle);
    free((void *)g.file_glob);
    char *out = ca_strbuf_detach(&g.sb);
    ca_tool_result *r = ca_tool_result_new(1, out ? out : "");
    free(out);
    return r;
}

const ca_tool *ca_tool_glob(void) {
    static const ca_tool t = {
        "glob",
        "Fast file pattern matching tool that works with any codebase size. Supports glob "
        "patterns like \"**/*.js\" or \"src/**/*.ts\" (** matches zero or more directories). "
        "Returns matching file paths sorted by modification time (newest first), truncated at 100. "
        "Use this tool when you need to find files by name patterns.",
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
        "\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}",
        0,
        glob_exec,
        NULL,
    };
    return &t;
}

const ca_tool *ca_tool_grep(void) {
    static const ca_tool t = {
        "grep",
        "A powerful search tool for file contents. ALWAYS use this for search tasks instead of "
        "grep/rg shell commands. Supports literal text patterns (case-insensitive with "
        "ignore_case). Filter files with the glob parameter (e.g. \"*.js\", \"**/*.tsx\"). "
        "Output modes: \"content\" shows matching lines as path:line:text, "
        "\"files_with_matches\" shows only file paths (default), \"count\" shows match counts. "
        "Skips .git and node_modules. head_limit caps output entries (default 250).",
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
        "\"path\":{\"type\":\"string\"},\"glob\":{\"type\":\"string\"},"
        "\"output_mode\":{\"type\":\"string\"},\"ignore_case\":{\"type\":\"boolean\"},"
        "\"head_limit\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}",
        0,
        grep_exec,
        NULL,
    };
    return &t;
}
