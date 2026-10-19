/* tool_search.c — glob and grep tools.
 * C ports of Claude Code's GlobTool (fast filename pattern matching, results
 * sorted by modification time, truncated at 100) and GrepTool (line-oriented
 * text search with files_with_matches / content / count output modes,
 * glob-filtered, head_limit capped). The pattern engine is a simplified
 * subset: glob supports *, ? and **; grep matches literal text (optionally
 * case-insensitive) instead of full ripgrep regex. */
#include "action/tools.h"
#include "os/os_fs.h"
#include "os/os_time.h"
#include "infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "cJSON.h"

/* ---------- shared path helpers ---------- */

static void resolve_root(const tool_ctx *ctx, const char *maybe_rel, char *out, size_t n) {
    if (maybe_rel && *maybe_rel)
        path_resolve(out, n, ctx ? ctx->workspace : NULL, maybe_rel);
    else if (ctx && ctx->workspace && *ctx->workspace)
        snprintf(out, n, "%s", ctx->workspace);
    else
        snprintf(out, n, ".");
    char *absolute = fs_absolute_path(out);
    if (absolute) { snprintf(out, n, "%s", absolute); free(absolute); }
}

static long long file_mtime(const char *path) {
#ifdef _WIN32
    /* paths are UTF-8; _stat interprets them as ANSI (GBK) — convert first */
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    struct _stat st;
    wchar_t *w;
    if (n <= 0)
        return 0;
    w = malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n) != n || _wstat(w, &st) != 0) {
        free(w);
        return 0;
    }
    free(w);
    return (long long)st.st_mtime;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return (long long)st.st_mtime;
#endif
}

static int ci_char(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* ---------- glob matching (subset of GlobTool / utils/glob) ----------
 * pattern segments are separated by '/'; "**" matches zero or more whole
 * segments; within a segment '*' matches any run and '?' one char. */

static int seg_match(const char *seg, size_t seglen, const char *s) {
    /* iterative wildcard match of one path segment against NUL-terminated s */
    const char *star = NULL, *ss = s, *star_s = NULL;
    const char *p = seg, *pe = seg + seglen;
    while (*ss) {
        if (p < pe && (*p == '?' || *p == *ss)) {
            p++;
            ss++;
        } else if (p < pe && *p == '*') {
            star = p++;
            star_s = ss;
        } else if (star) {
            p = star + 1;
            ss = ++star_s;
        } else
            return 0;
    }
    while (p < pe && *p == '*')
        p++;
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
            if (!*pat_rest)
                return 1; /* trailing ** matches everything left */
            /* try matching the remainder at every remaining position */
            while (1) {
                if (glob_match_segs(pat_rest, path_rest))
                    return 1;
                char *qslash = strchr(path_rest, '/');
                if (!qslash)
                    return 0;
                path_rest = qslash + 1;
            }
        }
        char *aslash = strchr(path_rest, '/');
        size_t alen = aslash ? (size_t)(aslash - path_rest) : strlen(path_rest);
        /* no length pre-check here: wildcard segments (*, ?) legitimately
         * match segments of a different length (e.g. "*.cpp" vs "main.cpp") */
        char tmp[256];
        if (alen >= sizeof(tmp))
            return 0;
        memcpy(tmp, path_rest, alen);
        tmp[alen] = '\0';
        if (!seg_match(pat_rest, plen, tmp))
            return 0;
        if (!pslash)
            return aslash == NULL;
        if (!aslash)
            return 0;
        pat_rest = pslash + 1;
        path_rest = aslash + 1;
    }

    return *path_rest == '\0';
}

typedef struct {
    char *path; /* relative path, '/'-separated */
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
        if (!ni)
            return;
        fl->items = ni;
        fl->cap = nc;
    }

    fl->items[fl->count].path = xstrdup(rel);
    fl->items[fl->count].mtime = mt;
    fl->count++;
}

/* newest first (like GlobTool); ties broken by path for determinism */
static int cmp_mtime_desc(const void *a, const void *b) {
    const found_entry *ea = (const found_entry *)a, *eb = (const found_entry *)b;
    if (ea->mtime != eb->mtime)
        return ea->mtime > eb->mtime ? -1 : 1;
    return strcmp(ea->path, eb->path);
}

static void walk_dir(const char *root, const char *rel, const char *pattern, found_list *fl) {
    char full[2048];
    dir_list dl;

    path_join(full, sizeof full, root, rel && *rel ? rel : "");
    memset(&dl, 0, sizeof dl);
    if (fs_list_dir(full, &dl) != 0)
        return;
    for (size_t i = 0; i < dl.count; i++) {
        const char *name = dl.items[i].name;
        if (!name || !*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child[1024];
        if (rel && *rel)
            snprintf(child, sizeof child, "%s/%s", rel, name);
        else
            snprintf(child, sizeof child, "%s", name);
        if (dl.items[i].is_dir) {
            if (strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0)
                continue;
            walk_dir(root, child, pattern, fl);
        } else {
            if (glob_match_segs((char *)pattern, child)) {
                char child_full[2048];
                path_join(child_full, sizeof child_full, root, child);
                found_push(fl, child, file_mtime(child_full));
            }
        }
    }

    fs_list_free(&dl);
}

static tool_result *glob_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *pat_j;
    cJSON *path_j;
    char root[1024];
    char *pat;
    const size_t LIMIT = 100;
    int truncated;
    strbuf sb;
    size_t shown;
    char *out;
    tool_result *r;

    (void)self;
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "glob: invalid args JSON");
    pat_j = cJSON_GetObjectItemCaseSensitive(args, "pattern");
    if (!pat_j || !cJSON_IsString(pat_j)) {
        cJSON_Delete(args);
        return tool_result_new(0, "glob: missing string arg 'pattern'");
    }

    path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    resolve_root(ctx, (path_j && cJSON_IsString(path_j)) ? path_j->valuestring : NULL, root, sizeof root);
    pat = xstrdup(pat_j->valuestring);
    cJSON_Delete(args);

    /* normalize pattern separators to '/' */
    for (char *p = pat; *p; p++)
        if (*p == '\\')
            *p = '/';

    found_list fl;
    memset(&fl, 0, sizeof fl);
    walk_dir(root, NULL, pat, &fl);
    free(pat);

    /* sort by modification time (newest first), like GlobTool */
    if (fl.count > 1)
        qsort(fl.items, fl.count, sizeof(found_entry), cmp_mtime_desc);
    truncated = fl.count > LIMIT;
    strbuf_init(&sb);
    if (fl.count == 0)
        strbuf_append(&sb, "No files found");
    shown = fl.count < LIMIT ? fl.count : LIMIT;
    for (size_t i = 0; i < shown; i++) {
        char full[2048]; path_join(full, sizeof(full), root, fl.items[i].path);
        for (char *p = full; *p; p++) if (*p == '\\') *p = '/';
        strbuf_appendf(&sb, "%s\n", full);
    }
    if (truncated)
        strbuf_appendf(&sb, "(Results are truncated from %zu. Consider using a more specific path or pattern.)\n",
                           fl.count);
    for (size_t i = 0; i < fl.count; i++)
        free(fl.items[i].path);
    free(fl.items);
    out = strbuf_detach(&sb);
    r = tool_result_new(1, out ? out : "");
    free(out);
    return r;
}

/* ---------- grep (subset of GrepTool) ---------- */

typedef struct {
    const char *needle; /* text to find */
    size_t needle_len;
    const char *file_glob; /* optional file-name glob filter, may be NULL */
    int ignore_case;
    int mode; /* 0 files_with_matches, 1 content, 2 count */
    size_t head_limit;
    strbuf sb; /* output */
    size_t out_n;  /* entries emitted */
    int truncated;
    size_t *prefix;
    const tool_ctx *ctx;
    const char *stop_reason;
    uint64_t deadline;
    size_t scanned_bytes, max_scan_bytes, scanned_files, max_files;
    int include_generated, skipped_generated, skipped_binary, cancelled, depth;
} grep_state;

static int grep_stop(grep_state *g) {
    if(g->stop_reason) return 1;
    if(g->ctx && g->ctx->should_abort && g->ctx->should_abort(g->ctx->abort_ud)) {
        g->cancelled=1;g->stop_reason="search cancelled";return 1;
    }
    if((uint64_t)time_now_ms()>=g->deadline) { g->stop_reason="search timeout; narrow the path or raise timeout_ms";return 1; }
    return 0;
}

/* Only an excerpt of an unusually long line is retained. Matching uses a
 * streaming prefix automaton, so matches across chunks/long lines are not lost. */
static int grep_line(const char *rel, size_t lineno, strbuf *line, size_t total,
                     int matched, size_t *hits, grep_state *g) {
    if(!matched) return 0;
    (*hits)++;
    if(g->mode==0) return 1;
    if(g->mode==1) {
        size_t n=line->len>8192 ? 8192 : line->len;
        while(n && ((unsigned char)line->buf[n]&0xc0)==0x80) --n;
        strbuf_appendf(&g->sb,"%s:%zu:",rel,lineno);
        strbuf_append_n(&g->sb,line->buf ? line->buf : "",n);
        if(total>n) strbuf_append(&g->sb," [line preview truncated; inspect file]");
        strbuf_append(&g->sb,"\n");g->out_n++;
        if(g->out_n>=g->head_limit) { g->stop_reason="head_limit reached; results may be truncated";return 1; }
    }
    return 0;
}

static void grep_file(const char *rel, const char *full, grep_state *g) {
    if(grep_stop(g)) return;
    if(g->scanned_files>=g->max_files) { g->stop_reason="file budget exhausted; narrow the path or raise max_files";return; }
    g->scanned_files++;
    uint64_t offset=0;size_t hits=0,lineno=1,total=0,matched_len=0;
    int matched=0,done=0;strbuf line;strbuf_init(&line);
    while(!done && !grep_stop(g)) {
        if(g->scanned_bytes>=g->max_scan_bytes) { g->stop_reason="byte budget exhausted; narrow the path or raise max_scan_bytes";break; }
        size_t want=g->max_scan_bytes-g->scanned_bytes;if(want>8192) want=8192;
        size_t count=0;int more=0;
        char *chunk=fs_read_file_slice(full,offset,want,&count,&more);
        if(!chunk) break;
        g->scanned_bytes+=count;offset+=count;
        if(memchr(chunk,0,count)) { g->skipped_binary++;free(chunk);strbuf_free(&line);return; }
        for(size_t i=0;i<count;i++) {
            unsigned char c=(unsigned char)chunk[i];
            if(c=='\n') {
                if(grep_line(rel,lineno,&line,total,matched,&hits,g)) { done=1;break; }
                lineno++;total=matched_len=0;matched=0;line.len=0;if(line.buf) line.buf[0]=0;
                continue;
            }
            total++;
            if(line.len<8196) strbuf_append_n(&line,chunk+i,1);
            if(g->ignore_case) c=(unsigned char)ci_char(c);
            while(matched_len && c!=(unsigned char)(g->ignore_case ? ci_char((unsigned char)g->needle[matched_len]) : g->needle[matched_len]))
                matched_len=g->prefix[matched_len-1];
            if(c==(unsigned char)(g->ignore_case ? ci_char((unsigned char)g->needle[matched_len]) : g->needle[matched_len])) matched_len++;
            if(matched_len==g->needle_len) { matched=1;matched_len=g->prefix[matched_len-1]; }
        }
        free(chunk);
        if(!more || !count) {
            if(!done && total) (void)grep_line(rel,lineno,&line,total,matched,&hits,g);
            break;
        }
    }
    if(hits && g->mode!=1 && (!g->stop_reason || g->mode==0)) {
        if(g->mode==0) strbuf_appendf(&g->sb,"%s\n",rel);
        else strbuf_appendf(&g->sb,"%s:%zu\n",rel,hits);
        if(++g->out_n>=g->head_limit) g->stop_reason="head_limit reached; results may be truncated";
    }
    strbuf_free(&line);
}

static int name_glob_ok(const char *name, const char *glob_pat) {
    char tmp[256];

    if (!glob_pat || !*glob_pat)
        return 1;
    snprintf(tmp, sizeof tmp, "%s", name);
    return glob_match_segs((char *)glob_pat, tmp);
}

static void grep_walk(const char *root, const char *rel, grep_state *g) {
    if(grep_stop(g)) return;
    if(g->depth>=64) { g->truncated=1;return; }
    char full[2048];
    if(rel && *rel) path_join(full,sizeof full,root,rel);
    else snprintf(full,sizeof full,"%s",root);
    if (fs_is_dir(full)) {
        dir_list dl;
        memset(&dl, 0, sizeof dl);
        if (fs_list_dir(full, &dl) != 0)
            return;
        for (size_t i = 0; i < dl.count && !grep_stop(g); i++) {
            const char *name = dl.items[i].name;
            if (!name || !*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            if (strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0)
                continue;
            if(dl.items[i].is_dir && !g->include_generated &&
               (!strcmp(name,"build") || !strcmp(name,"dist") || !strcmp(name,".tool-results") ||
                !strcmp(name,".zig-cache") || !strcmp(name,".venv") || !strcmp(name,"__pycache__"))) {
                g->skipped_generated++;continue;
            }
            char child[1024];
            if (rel && *rel)
                snprintf(child, sizeof child, "%s/%s", rel, name);
            else
                snprintf(child, sizeof child, "%s", name);
            g->depth++;grep_walk(root, child, g);g->depth--;
        }
        fs_list_free(&dl);
    } else {
        /* file: apply the optional glob name filter. rel is NULL on the
         * top-level call; when the resolved root itself is a FILE (the
         * model plans grep with path=<a .py file>) the old
         * strrchr(rel, '/') dereferenced NULL and crashed the server
         * (SWE-bench sympy instances died mid-run with SIGSEGV in
         * libc). Guard it and display the resolved path instead. */
        const char *disp = rel ? rel : full;
        const char *base = strrchr(disp, '/');
        const char *backslash = strrchr(disp, '\\');
        if(backslash && (!base || backslash>base)) base=backslash;
        base = base ? base + 1 : disp;
        if (!name_glob_ok(base, g->file_glob))
            return;
        grep_file(disp, full, g);
    }
}

static size_t grep_budget(cJSON *args,const char *name,size_t fallback,size_t maximum) {
    cJSON *v=cJSON_GetObjectItemCaseSensitive(args,name);
    if(!cJSON_IsNumber(v) || v->valuedouble<=0) return fallback;
    return v->valuedouble>(double)maximum ? maximum : (size_t)v->valuedouble;
}

static tool_result *grep_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *pat_j;
    cJSON *path_j;
    cJSON *glob_j;
    cJSON *mode_j;
    cJSON *ic_j;
    cJSON *hl_j;
    char root[1024];
    char *out;
    tool_result *r;

    (void)self;
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "grep: invalid args JSON");
    pat_j = cJSON_GetObjectItemCaseSensitive(args, "pattern");
    if (!pat_j || !cJSON_IsString(pat_j)) {
        cJSON_Delete(args);
        return tool_result_new(0, "grep: missing string arg 'pattern'");
    }

    path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    glob_j = cJSON_GetObjectItemCaseSensitive(args, "glob");
    mode_j = cJSON_GetObjectItemCaseSensitive(args, "output_mode");
    ic_j = cJSON_GetObjectItemCaseSensitive(args, "ignore_case");
    hl_j = cJSON_GetObjectItemCaseSensitive(args, "head_limit");

    grep_state g;
    memset(&g, 0, sizeof g);
    /* copy strings out before cJSON_Delete(args) frees the tree */
    g.needle = xstrdup(pat_j->valuestring);
    g.needle_len = strlen(g.needle);
    if(!g.needle_len || g.needle_len>65536) {
        free((void *)g.needle);cJSON_Delete(args);return tool_result_new(0,"grep: use a non-empty literal pattern of at most 65536 bytes");
    }
    g.prefix=calloc(g.needle_len,sizeof(*g.prefix));
    if(!g.prefix) { free((void *)g.needle);cJSON_Delete(args);return tool_result_new(0,"grep: allocation failed"); }
    g.ctx=ctx;
    g.max_scan_bytes=grep_budget(args,"max_scan_bytes",64*1024*1024,1024*1024*1024);
    g.max_files=grep_budget(args,"max_files",10000,1000000);
    g.deadline=(uint64_t)time_now_ms()+grep_budget(args,"timeout_ms",5000,30000);
    g.include_generated=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args,"include_generated"));
    g.file_glob = (glob_j && cJSON_IsString(glob_j) && *glob_j->valuestring) ? xstrdup(glob_j->valuestring) : NULL;
    g.ignore_case = (ic_j && cJSON_IsTrue(ic_j)) ? 1 : 0;
    for(size_t i=1,k=0;i<g.needle_len;i++) {
        unsigned char c=(unsigned char)(g.ignore_case ? ci_char((unsigned char)g.needle[i]) : g.needle[i]);
        while(k && c!=(unsigned char)(g.ignore_case ? ci_char((unsigned char)g.needle[k]) : g.needle[k])) k=g.prefix[k-1];
        if(c==(unsigned char)(g.ignore_case ? ci_char((unsigned char)g.needle[k]) : g.needle[k])) ++k;
        g.prefix[i]=k;
    }
    g.mode = 0;
    if (mode_j && cJSON_IsString(mode_j)) {
        if (strcmp(mode_j->valuestring, "content") == 0)
            g.mode = 1;
        else if (strcmp(mode_j->valuestring, "count") == 0)
            g.mode = 2;
    }

    g.head_limit = (hl_j && cJSON_IsNumber(hl_j) && hl_j->valuedouble > 0) ? (size_t)hl_j->valuedouble : 250;

    resolve_root(ctx, (path_j && cJSON_IsString(path_j)) ? path_j->valuestring : NULL, root, sizeof root);
    cJSON_Delete(args);

    strbuf_init(&g.sb);
    grep_walk(root, NULL, &g);
    if (g.out_n == 0 && !g.stop_reason && !g.truncated)
        strbuf_append(&g.sb, "No matches found (literal text matching, not regular expressions). "
            "If pattern uses alternatives such as a|b, issue separate literal searches for a and b; "
            "do not infer that those individual symbols are absent. Use a specific symbol instead of reading the whole file.\n");
    if(g.stop_reason) strbuf_appendf(&g.sb,"[Search stopped: %s]\n",g.stop_reason);
    if(g.skipped_generated) strbuf_append(&g.sb,"[Generated directories skipped; use their explicit path or include_generated=true to search them.]\n");
    if(g.skipped_binary) strbuf_append(&g.sb,"[Binary files skipped.]\n");
    if(g.truncated) strbuf_append(&g.sb,"[Search incomplete: directory nesting exceeded 64.]\n");
    free(g.prefix);free((void *)g.needle);
    free((void *)g.file_glob);
    out = strbuf_detach(&g.sb);
    int ok=!g.cancelled && !g.truncated && (!g.stop_reason || !strncmp(g.stop_reason,"head_limit",10));
    r = tool_result_new(ok, out ? out : "");
    free(out);
    return r;
}

const tool *tool_glob(void) {
    static const tool t = {
        "glob",
        "Fast file pattern matching tool that works with any codebase size. Supports glob "
        "patterns like \"**/*.js\" or \"src/**/*.ts\" (** matches zero or more directories). "
        "Returns matching file paths sorted by modification time (newest first), truncated at 100. "
        "Paths are absolute and can be passed unchanged to file_read; preserve their search root. "
        "Use this tool when you need to find files by name patterns.",
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
        "\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}",
        0,
        glob_exec,
        NULL,
    };
    return &t;
}

const tool *tool_grep(void) {
    static const tool t = {
        "grep",
        "A powerful search tool for file contents. ALWAYS use this for search tasks instead of "
        "grep/rg shell commands. Supports literal text patterns (case-insensitive with "
        "ignore_case); regex operators such as |, .* and (...) are not interpreted. "
        "Batch separate literal keyword searches for alternatives. Filter files with the glob parameter (e.g. \"*.js\", \"**/*.tsx\"). "
        "Output modes: \"content\" shows matching lines as path:line:text, "
        "\"files_with_matches\" shows only file paths (default), \"count\" shows match counts. "
        "Stops scanning at head_limit (default 250). Skips binary and generated directories "
        "(build/dist/.tool-results/.zig-cache/.venv/__pycache__); explicit root paths remain searchable "
        "or set include_generated=true. Per-search budgets default to 5 seconds, 10000 files and 64 MiB; "
        "incomplete searches report a failure so narrow the path or adjust timeout_ms/max_files/max_scan_bytes.",
        "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},"
        "\"path\":{\"type\":\"string\"},\"glob\":{\"type\":\"string\"},"
        "\"output_mode\":{\"type\":\"string\"},\"ignore_case\":{\"type\":\"boolean\"},"
        "\"head_limit\":{\"type\":\"integer\"},\"include_generated\":{\"type\":\"boolean\"},"
        "\"timeout_ms\":{\"type\":\"integer\"},\"max_files\":{\"type\":\"integer\"},"
        "\"max_scan_bytes\":{\"type\":\"integer\"}},\"required\":[\"pattern\"]}",
        0,
        grep_exec,
        NULL,
    };
    return &t;
}
