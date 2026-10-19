/* tool_file.c — file_read and file_write tools. */
#include "action/tools.h"
#include "os/os_fs.h"
#include "infra/util.h"
#include "os/os_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "cJSON.h"

/* Bounded process-local LRU of immutable file pages. The backing file remains
 * authoritative: every lookup checks its current revision before returning a
 * page, so external edits invalidate cached bytes automatically. */
#define FILE_PAGE_CACHE_ENTRIES 256
#define FILE_PAGE_CACHE_BYTES (8u * 1024u * 1024u)
typedef struct file_page_entry {
    char *session, *path, *data;
    uint64_t version, offset, tick;
    size_t page_bytes, length;
    int has_more, used;
} file_page_entry;
static file_page_entry g_file_pages[FILE_PAGE_CACHE_ENTRIES];
static size_t g_file_pages_bytes;
static uint64_t g_file_page_tick;
static mutex_t g_file_page_mutex;
static atomic_int g_file_page_mutex_ready;
static atomic_flag g_file_page_init_lock = ATOMIC_FLAG_INIT;

static void file_page_lock(void) {
    if (!atomic_load(&g_file_page_mutex_ready)) {
        while (atomic_flag_test_and_set(&g_file_page_init_lock)) { }
        if (!atomic_load(&g_file_page_mutex_ready)) {
            mutex_init(&g_file_page_mutex);
            atomic_store(&g_file_page_mutex_ready, 1);
        }
        atomic_flag_clear(&g_file_page_init_lock);
    }
    mutex_lock(&g_file_page_mutex);
}
static void file_page_drop(file_page_entry *e) {
    if (!e->used) return;
    g_file_pages_bytes -= e->length;
    free(e->session); free(e->path); free(e->data);
    memset(e, 0, sizeof(*e));
}
static int file_page_same(const file_page_entry *e, const char *session,
                          const char *path, uint64_t version,
                          uint64_t offset, size_t page_bytes) {
    return e->used && e->version == version && e->offset == offset &&
           e->page_bytes == page_bytes && !strcmp(e->session, session) &&
           !strcmp(e->path, path);
}
static int file_page_get(const char *session, const char *path, uint64_t version,
                         uint64_t offset, size_t page_bytes, char **data,
                         size_t *length, int *has_more) {
    int hit = 0;
    file_page_lock();
    for (size_t i=0; i<FILE_PAGE_CACHE_ENTRIES; ++i) {
        file_page_entry *e = &g_file_pages[i];
        if (e->used && !strcmp(e->session, session) && !strcmp(e->path, path) &&
            e->version != version) file_page_drop(e);
        if (file_page_same(e,session,path,version,offset,page_bytes)) {
            *data = malloc(e->length + 1);
            if (*data) {
                memcpy(*data,e->data,e->length+1); *length=e->length;
                *has_more=e->has_more; e->tick=++g_file_page_tick; hit=1;
            }
            break;
        }
    }
    mutex_unlock(&g_file_page_mutex);
    return hit;
}
static void file_page_put(const char *session, const char *path, uint64_t version,
                          uint64_t offset, size_t page_bytes, const char *data,
                          size_t length, int has_more) {
    if (!session || !*session || length > FILE_PAGE_CACHE_BYTES) return;
    char *s=xstrdup(session), *p=xstrdup(path), *d=malloc(length+1);
    if (!s || !p || !d) { free(s); free(p); free(d); return; }
    memcpy(d,data,length); d[length]=0;
    file_page_lock();
    size_t slot=FILE_PAGE_CACHE_ENTRIES;
    for (size_t i=0;i<FILE_PAGE_CACHE_ENTRIES;++i) {
        file_page_entry *e=&g_file_pages[i];
        if (e->used && !strcmp(e->session,session) && !strcmp(e->path,path) && e->version!=version)
            file_page_drop(e);
        if (file_page_same(e,session,path,version,offset,page_bytes)) { slot=i; break; }
        if (!e->used && slot==FILE_PAGE_CACHE_ENTRIES) slot=i;
    }
    if (slot==FILE_PAGE_CACHE_ENTRIES) {
        uint64_t oldest=UINT64_MAX;
        for(size_t i=0;i<FILE_PAGE_CACHE_ENTRIES;++i)
            if(g_file_pages[i].tick<oldest){oldest=g_file_pages[i].tick;slot=i;}
    }
    if (slot<FILE_PAGE_CACHE_ENTRIES) {
        file_page_drop(&g_file_pages[slot]);
        while (g_file_pages_bytes + length > FILE_PAGE_CACHE_BYTES) {
            size_t victim=FILE_PAGE_CACHE_ENTRIES; uint64_t oldest=UINT64_MAX;
            for(size_t i=0;i<FILE_PAGE_CACHE_ENTRIES;++i)
                if(g_file_pages[i].used && i!=slot && g_file_pages[i].tick<oldest){oldest=g_file_pages[i].tick;victim=i;}
            if(victim==FILE_PAGE_CACHE_ENTRIES) break;
            file_page_drop(&g_file_pages[victim]);
        }
        file_page_entry *e=&g_file_pages[slot];
        e->session=s;e->path=p;e->data=d;e->version=version;e->offset=offset;
        e->page_bytes=page_bytes;e->length=length;e->has_more=has_more;e->tick=++g_file_page_tick;e->used=1;
        g_file_pages_bytes+=length; s=p=d=NULL;
    }
    mutex_unlock(&g_file_page_mutex);
    free(s);free(p);free(d);
}
static void file_page_invalidate_path(const char *path) {
    char *absolute=fs_absolute_path(path);
    const char *target=absolute ? absolute : path;
    file_page_lock();
    for(size_t i=0;i<FILE_PAGE_CACHE_ENTRIES;++i)
        if(g_file_pages[i].used && !strcmp(g_file_pages[i].path,target))
            file_page_drop(&g_file_pages[i]);
    mutex_unlock(&g_file_page_mutex);
    free(absolute);
}

char *tool_file_cached_content(const char *session_id, const char *absolute_path) {
    if (!session_id || !*session_id || !absolute_path) return NULL;
    uint64_t version=fs_file_version(absolute_path);
    if (!version) return NULL;
    strbuf out; strbuf_init(&out);
    uint64_t offset=0; int complete=0;
    for (size_t page=0; page<FILE_PAGE_CACHE_ENTRIES; ++page) {
        int found=0; char *copy=NULL; size_t len=0; int more=0;
        file_page_lock();
        for(size_t i=0;i<FILE_PAGE_CACHE_ENTRIES;++i){
            file_page_entry *e=&g_file_pages[i];
            if(e->used && e->version==version && e->offset==offset &&
               !strcmp(e->session,session_id) && !strcmp(e->path,absolute_path)){
                copy=malloc(e->length+1);
                if(copy){memcpy(copy,e->data,e->length+1);len=e->length;more=e->has_more;e->tick=++g_file_page_tick;found=1;}
                break;
            }
        }
        mutex_unlock(&g_file_page_mutex);
        if(!found){free(copy);break;}
        strbuf_append_n(&out,copy,len);free(copy);offset+=len;
        if(!more){complete=1;break;}
    }
    if(!complete){strbuf_free(&out);return NULL;}
    return strbuf_detach(&out);
}

static int cached_line_relevant(const char *line, size_t len, const char *query) {
    if (!line || !query || !*query) return 0;
    for (const unsigned char *q=(const unsigned char *)query; *q;) {
        if ((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='_'||*q=='-') {
            char term[96]; size_t n=0;
            while (((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='_'||*q=='-') && n+1<sizeof(term)) term[n++]=(char)*q++;
            term[n]=0;
            if (n>=3) {
                for(size_t i=0;i+n<=len;++i) if(!memcmp(line+i,term,n)) return 1;
            }
        } else if (*q>=0xe0) {
            size_t n=(*q>=0xf0)?4:3;
            const unsigned char *next=q+n;
            if (!next[0]) break;
            if (*next>=0xe0) {
                for(size_t i=0;i+2*n<=len;++i) if(!memcmp(line+i,q,n) && !memcmp(line+i+n,next,n)) return 1;
            }
            q=next;
        } else q++;
    }
    return 0;
}

static int excerpt_already_present(const char *text, const char *line, size_t n) {
    if(!text || !n) return 0;
    const char *end=text+strlen(text);
    for(const char *p=text; p<end && (p=memchr(p,*line,(size_t)(end-p)))!=NULL; ++p)
        if((size_t)(end-p)>=n && !memcmp(p,line,n)) return 1;
    return 0;
}

char *tool_file_cache_context(const char *session_id, const char *query, size_t max_bytes) {
    return tool_file_cache_context_excluding(session_id,query,max_bytes,NULL);
}

char *tool_file_cache_context_excluding(const char *session_id, const char *query, size_t max_bytes,
                                       const char *recent_observations) {
    if (!session_id || !*session_id || !query || !*query || !max_bytes) return NULL;
    strbuf out; strbuf_init(&out);
    file_page_lock();
    for (size_t i=0;i<FILE_PAGE_CACHE_ENTRIES && out.len<max_bytes;++i) {
        file_page_entry *e=&g_file_pages[i];
        if (!e->used || strcmp(e->session,session_id) || !e->length) continue;
        if (fs_file_version(e->path) != e->version) { file_page_drop(e); continue; }
        const char *p=e->data; size_t line_no=0;
        while (*p && out.len<max_bytes) {
            const char *end=strchr(p,'\n'); if(!end) end=p+strlen(p);
            size_t n=(size_t)(end-p);
            if (n && cached_line_relevant(p,n,query) && !excerpt_already_present(recent_observations,p,n)) {
                char header[2200];
                int hn=snprintf(header,sizeof(header),"- %s@%llu:%zu: ",e->path,(unsigned long long)e->offset,line_no);
                size_t remain=max_bytes-out.len;
                if(hn<0) break;
                if((size_t)hn>=remain){strbuf_append_n(&out,header,remain);break;}
                strbuf_append_n(&out,header,(size_t)hn);
                remain=max_bytes-out.len;
                size_t take=n;
                if(take>=remain) take=remain ? remain-1 : 0;
                if(take) strbuf_append_n(&out,p,take);
                if(out.len<max_bytes) strbuf_append(&out,"\n");
                e->tick=++g_file_page_tick;
            }
            if(!*end) break;
            p=end+1; line_no++;
        }
    }
    mutex_unlock(&g_file_page_mutex);
    if(!out.len){strbuf_free(&out);return NULL;}
    return strbuf_detach(&out);
}

/* Resolve a (possibly relative) path against the workspace. Caller frees. */
static char *resolve_path(const tool_ctx *ctx, const char *path) {
    char full[2048];
    path_resolve(full, sizeof(full), ctx ? ctx->workspace : NULL, path ? path : "");
    return xstrdup(full);
}

static tool_result *file_read_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *path_j;
    char *rp;
    char *content;
    tool_result *r;
    size_t read_bytes = 0;
    int has_more = 0;
    uint64_t offset = 0;
    size_t page_bytes = 4000;
    int clamped_page = 0;
    int cache_hit = 0;

    (void)self;
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "file_read: invalid args JSON");
    path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    cJSON *size_j=cJSON_GetObjectItemCaseSensitive(args,"max_bytes");
    if (size_j) {
        if (!cJSON_IsNumber(size_j) || size_j->valuedouble<4 || size_j->valuedouble>9007199254740991.0 ||
            size_j->valuedouble != (double)(uint64_t)size_j->valuedouble) {
            cJSON_Delete(args); return tool_result_new(0,"file_read: max_bytes must be a positive integer >= 4; use 4..4000 per page");
        }
        clamped_page = size_j->valuedouble > 4000;
        page_bytes = clamped_page ? 4000 : (size_t)size_j->valuedouble;
    }
    if (!path_j || !cJSON_IsString(path_j)) {
        cJSON_Delete(args);
        return tool_result_new(0, "file_read: missing string arg 'path'");
    }
    cJSON *offset_j = cJSON_GetObjectItemCaseSensitive(args, "offset_bytes");
    if (offset_j && (!cJSON_IsNumber(offset_j) || offset_j->valuedouble < 0 ||
        offset_j->valuedouble > 9007199254740991.0 ||
        offset_j->valuedouble != (double)(uint64_t)offset_j->valuedouble)) {
        cJSON_Delete(args); return tool_result_new(0,"file_read: offset_bytes must be a non-negative integer; copy next_offset_bytes from the previous page");
    }
    if (offset_j)
        offset = (uint64_t)offset_j->valuedouble;

    rp = resolve_path(ctx, path_j->valuestring);
    if (!rp || !*rp) {
        free(rp); cJSON_Delete(args);
        return tool_result_new(0, "file_read: path is empty or too long");
    }
    char *absolute_path = fs_absolute_path(rp);
    if (absolute_path) { free(rp); rp = absolute_path; }
    /* Reading a directory is common (the planner often probes a path before
     * reading a file). Instead of failing, return a listing so the agent can
     * continue — a single mis-targeted file_read must not fail the whole task. */
    if (fs_is_dir(rp)) {
        dir_list dl;
        memset(&dl, 0, sizeof(dl));
        if (fs_list_dir(rp, &dl) == 0) {
            strbuf sb;
            strbuf_init(&sb);
            strbuf_appendf(&sb, "Directory listing of %s:\n", rp);
            for (size_t i = 0; i < dl.count; i++) {
                char full[2048]; path_join(full, sizeof(full), rp, dl.items[i].name);
                for (char *p = full; *p; p++) if (*p == '\\') *p = '/';
                strbuf_appendf(&sb, "%s%s\n", full, dl.items[i].is_dir ? "/" : "");
            }
            fs_list_free(&dl);
            char *out = strbuf_detach(&sb);
            free(rp);
            cJSON_Delete(args);
            tool_result *r = tool_result_new(1, out);
            free(out);
            return r;
        }
        fs_list_free(&dl);
    }

    /* Leave space for metadata and tool wrappers in the 6144-byte observation
     * budget. A page must reach the model intact, rather than lose its middle. */
    const char *session = ctx && ctx->session_id && *ctx->session_id ? ctx->session_id : "";
    uint64_t version = fs_file_version(rp);
    if (version && *session)
        cache_hit = file_page_get(session, rp, version, offset, page_bytes,
                                  &content, &read_bytes, &has_more);
    if (!cache_hit)
        content = fs_read_file_slice(rp, offset, page_bytes, &read_bytes, &has_more);
    cJSON_Delete(args);
    if (!content) {
        char msg[4096];
        char *absolute = fs_absolute_path(rp);
        snprintf(msg, sizeof(msg), "file_read: %s %s. Relative paths resolve against workspace '%s'. "
                 "Do not retry the same missing path; use glob/list_dir to discover its actual location "
                 "(headers may be under include/, not src/) and pass the returned absolute path unchanged.",
                 fs_exists(rp) ? "cannot read" : "path does not exist:", absolute ? absolute : rp,
                 ctx && ctx->workspace ? ctx->workspace : ".");
        free(absolute);
        free(rp);
        return tool_result_new(0, msg);
    }

    if (memchr(content, '\0', read_bytes)) {
        free(rp); free(content);
        return tool_result_new(0, "file_read: binary file; use a format-specific extractor");
    }
    if (!cache_hit && version && *session)
        file_page_put(session, rp, version, offset, page_bytes, content,
                      read_bytes, has_more);
    char *sanitized = NULL;
    if (has_more && read_bytes && !str_utf8_valid_n(content, (int)read_bytes)) {
        size_t lead = read_bytes-1;
        while (lead && ((unsigned char)content[lead] & 0xc0) == 0x80) --lead;
        unsigned char c = (unsigned char)content[lead];
        size_t width = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xc2 && c <= 0xdf ? 2 : 1;
        if (lead+width > read_bytes && str_utf8_valid_n(content, (int)lead)) {
            read_bytes = lead; content[lead] = 0;
        }
    }
    if (!str_utf8_valid_n(content, (int)read_bytes))
        sanitized = str_utf8_sanitize(content);
    strbuf output;
    strbuf_init(&output);
    if (offset || has_more || clamped_page)
        strbuf_appendf(&output, "[bytes %llu-%llu%s; next_offset_bytes=%llu; file_page_cache=%s; file_version=%llu]\n",
                       (unsigned long long)offset,
                       (unsigned long long)(offset + (read_bytes ? read_bytes - 1 : 0)),
                       has_more ? ", more available" : ", end of file",
                       (unsigned long long)(offset + read_bytes),
                       cache_hit ? "HIT" : "MISS", (unsigned long long)version);
    if (clamped_page)
        strbuf_append(&output, "[max_bytes capped to 4000; continue at next_offset_bytes, not the requested page size]\n");
    if (sanitized) strbuf_append(&output, sanitized);
    else strbuf_append_n(&output, content, read_bytes);
    char *shown = strbuf_detach(&output);
    free(rp);
    r = tool_result_new(1, shown ? shown : content);
    free(shown);
    free(sanitized);
    free(content);
    return r;
}

static tool_result *file_write_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *path_j;
    cJSON *content_j;
    char *rp;
    /* ensure parent dir */
    char *slash;
    size_t content_len;
    int w;
    char out[1200];

    (void)self;
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "file_write: invalid args JSON");
    path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    content_j = cJSON_GetObjectItemCaseSensitive(args, "content");
    if (!path_j || !cJSON_IsString(path_j)) {
        cJSON_Delete(args);
        return tool_result_new(0, "file_write: missing string arg 'path'");
    }

    const char *content = (content_j && cJSON_IsString(content_j)) ? content_j->valuestring : "";

    /* Models sometimes emit a literal "..." placeholder instead of the real
     * file body (it even overwrote a generated header once). Reject the
     * obvious cases so the loop retries with actual content. */
    {
        size_t clen = strlen(content);
        int only_dots = clen > 0;
        for (size_t i = 0; i < clen && only_dots; i++)
            if (content[i] != '.' && content[i] != '\n' && content[i] != '\r')
                only_dots = 0;
        if (only_dots || (clen < 32 && strstr(content, "..."))) {
            cJSON_Delete(args);
            return tool_result_new(0,
                                   "file_write: content looks like a placeholder (\"...\") — "
                                   "rewrite the action with the FULL file content");
        }
    }

    rp = resolve_path(ctx, path_j->valuestring);
    /* ensure parent dir */
    slash = strrchr(rp, '/');
#if defined(_WIN32)
    char *bslash = strrchr(rp, '\\');
    if (bslash && (!slash || bslash > slash))
        slash = bslash;
#endif
    if (slash) {
        char dir[2048];
        size_t n = (size_t)(slash - rp);
        if (n > 0) {
            snprintf(dir, sizeof(dir), "%.*s", (int)n, rp);
            fs_mkdirs(dir);
        }
    }

    content_len = strlen(content);
    w = fs_write_file(rp, content, content_len);
    cJSON_Delete(args);
    if (w != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "file_write: failed to write %s", rp);
        free(rp);
        return tool_result_new(0, msg);
    }

    file_page_invalidate_path(rp);

    snprintf(out, sizeof(out), "wrote %zu bytes to %s", content_len, rp);
    free(rp);
    return tool_result_new(1, out);
}

/* file_edit — exact string replacement (ported from Claude Code FileEditTool):
 * fails when old_string is absent, or when it occurs more than once unless
 * replace_all is set. */
static size_t count_occurrences(const char *hay, const char *needle) {
    size_t n = 0;
    size_t nl = strlen(needle);
    const char *p = hay;

    if (nl == 0)
        return 0;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += nl;
    }

    return n;
}

static char *with_crlf(const char *text) {
    size_t extra = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n' && (p == text || p[-1] != '\r'))
            extra++;
    char *out = malloc(strlen(text) + extra + 1);
    if (!out)
        return NULL;
    char *w = out;
    for (const char *p = text; *p; p++) {
        if (*p == '\n' && (p == text || p[-1] != '\r'))
            *w++ = '\r';
        *w++ = *p;
    }
    *w = '\0';
    return out;
}

static tool_result *file_edit_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *path_j;
    cJSON *old_j;
    cJSON *new_j;
    int replace_all = 0;
    cJSON *ra_j;
    /* copy strings out before cJSON_Delete(args) frees the tree */
    char *old_s;
    char *new_s;
    char *rp;
    char *content;
    size_t old_len;
    size_t occ;
    strbuf sb;
    int w;
    char out[1240];

    (void)self;
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "file_edit: invalid args JSON");
    path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    old_j = cJSON_GetObjectItemCaseSensitive(args, "old_string");
    new_j = cJSON_GetObjectItemCaseSensitive(args, "new_string");
    if (!path_j || !cJSON_IsString(path_j) || !old_j || !cJSON_IsString(old_j) || !new_j || !cJSON_IsString(new_j)) {
        cJSON_Delete(args);
        return tool_result_new(0, "file_edit: requires string args 'path', 'old_string', 'new_string'");
    }

    ra_j = cJSON_GetObjectItemCaseSensitive(args, "replace_all");
    if (ra_j && cJSON_IsTrue(ra_j))
        replace_all = 1;

    /* copy strings out before cJSON_Delete(args) frees the tree */
    old_s = xstrdup(old_j->valuestring);
    new_s = xstrdup(new_j->valuestring);
    rp = resolve_path(ctx, path_j->valuestring);
    content = fs_read_file(rp);
    cJSON_Delete(args);
    if (!content) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "file_edit: file does not exist: %s (use file_write to create it)", rp);
        free(rp);
        free(old_s);
        free(new_s);
        return tool_result_new(0, msg);
    }

    old_len = strlen(old_s);
    occ = count_occurrences(content, old_s);
    /* LLM JSON conventionally uses LF while files read on Windows often keep
     * CRLF. Exact edit semantics still apply after adapting only line endings. */
    if (occ == 0 && strchr(old_s, '\n') && strstr(content, "\r\n")) {
        char *crlf_old = with_crlf(old_s);
        char *crlf_new = with_crlf(new_s);
        if (crlf_old && crlf_new && count_occurrences(content, crlf_old) > 0) {
            free(old_s); free(new_s);
            old_s = crlf_old; new_s = crlf_new;
            old_len = strlen(old_s);
            occ = count_occurrences(content, old_s);
        } else {
            free(crlf_old); free(crlf_new);
        }
    }
    if (occ == 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg),
                 "file_edit: old_string not found in %s (the edit will fail if old_string does not match exactly, "
                 "including whitespace)",
                 rp);
        free(content);
        free(rp);
        free(old_s);
        free(new_s);
        return tool_result_new(0, msg);
    }

    if (occ > 1 && !replace_all) {
        char msg[1200];
        snprintf(msg, sizeof(msg),
                 "file_edit: old_string is not unique in %s (%zu occurrences). Provide a larger string with more "
                 "surrounding context to make it unique, or use replace_all to change every instance",
                 rp, occ);
        free(content);
        free(rp);
        free(old_s);
        free(new_s);
        return tool_result_new(0, msg);
    }

    strbuf_init(&sb);
    const char *p = content;
    while (*p) {
        const char *hit = strstr(p, old_s);
        if (!hit) {
            strbuf_append(&sb, p);
            break;
        }
        strbuf_append_n(&sb, p, (size_t)(hit - p));
        strbuf_append(&sb, new_s);
        p = hit + old_len;
    }

    w = fs_write_file(rp, sb.buf ? sb.buf : "", sb.len);
    free(sb.buf);
    free(content);
    free(old_s);
    free(new_s);
    if (w != 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "file_edit: failed to write %s", rp);
        free(rp);
        return tool_result_new(0, msg);
    }

    file_page_invalidate_path(rp);

    snprintf(out, sizeof(out), "edited %s (%zu replacement%s)", rp, occ, occ == 1 ? "" : "s");
    free(rp);
    return tool_result_new(1, out);
}

static tool_result *list_dir_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    (void)self;
    cJSON *args = cJSON_Parse(args_json ? args_json : "{}");
    if (!args || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        return tool_result_new(0, "list_dir: invalid args JSON");
    }
    cJSON *path_j = cJSON_GetObjectItemCaseSensitive(args, "path");
    const char *path = path_j && cJSON_IsString(path_j) ? path_j->valuestring : ".";
    char *rp = resolve_path(ctx, path);
    char *absolute = rp ? fs_absolute_path(rp) : NULL;
    if (absolute) { free(rp); rp = absolute; }
    cJSON_Delete(args);
    if (!rp || !fs_is_dir(rp)) {
        free(rp);
        return tool_result_new(0, "list_dir: directory does not exist");
    }
    dir_list dl = {0};
    if (fs_list_dir(rp, &dl) != 0) {
        free(rp);
        return tool_result_new(0, "list_dir: cannot read directory");
    }
    strbuf sb;
    strbuf_init(&sb);
    strbuf_appendf(&sb, "Directory listing of %s:\n", rp);
    for (size_t i = 0; i < dl.count; i++) {
        char full[2048]; path_join(full, sizeof(full), rp, dl.items[i].name);
        for (char *p = full; *p; p++) if (*p == '\\') *p = '/';
        strbuf_appendf(&sb, "%s%s\n", full, dl.items[i].is_dir ? "/" : "");
    }
    fs_list_free(&dl);
    free(rp);
    char *out = strbuf_detach(&sb);
    tool_result *r = tool_result_new(1, out ? out : "");
    free(out);
    return r;
}

const tool *tool_list_dir(void) {
    static const tool t = {
        "list_dir",
        "List files and subdirectories in a directory (default: workspace root).",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}",
        0,
        list_dir_exec,
        NULL,
    };
    return &t;
}

const tool *tool_file_read(void) {
    static const tool t = {
        "file_read",
        "Read text in pages of 4..4000 bytes (default 4000). Oversized integer max_bytes is capped to 4000. "
        "Continue using the returned next_offset_bytes, never offset + requested max_bytes. "
        "Pass discovered absolute paths unchanged. Directories return listings.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset_bytes\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":9007199254740991},\"max_bytes\":{\"type\":\"integer\",\"minimum\":4,\"maximum\":4000,\"default\":4000}},\"required\":[\"path\"]}",
        0,
        file_read_exec,
        NULL,
    };
    return &t;
}

const tool *tool_file_write(void) {
    static const tool t = {
        "file_write",
        "Write content to a file (creates parent directories).",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}}}",
        1,
        file_write_exec,
        NULL,
    };
    return &t;
}

const tool *tool_file_edit(void) {
    static const tool t = {
        "file_edit",
        "Performs exact string replacements in files. ALWAYS prefer editing existing files; "
        "NEVER write new files unless required. The edit FAILS if old_string is not unique in the "
        "file - either provide a larger string with more surrounding context to make it unique, or "
        "use replace_all to change every instance of old_string.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"old_string\":{\"type\":\"string\"},\"new_string\":{\"type\":\"string\"},"
        "\"replace_all\":{\"type\":\"boolean\"}},"
        "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        1,
        file_edit_exec,
        NULL,
    };
    return &t;
}
