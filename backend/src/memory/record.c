/* record.c — V3.0 long-term memory records: Markdown-canonical store with a
 * rebuildable derived index (Architecture Baseline §10, §4.5).
 *
 * Storage layout: one file per record under <root>/records/mem_<hex>.md
 *   ---
 *   id: mem_ab12cd
 *   type: procedural
 *   scope: agent/status
 *   status: active
 *   importance: 0.94
 *   ...
 *   ---
 *   <content body>
 * The frontmatter is a deliberately tiny YAML subset: flat "key: value"
 * lines plus one nested "emotion:" / "source:" block. Both the writer and
 * the parser live here, so the subset stays under control. */
#include "memory/record.h"
#include "memory/vector.h"
#include "infra/util.h"
#include "security/secret.h"
#include "os/os_fs.h"
#include "os/os_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* --- small helpers ----------------------------------------------------- */

static const char *type_name(mem_type_t t) {
    switch (t) {
    case MEMR_SEMANTIC:   return "semantic";
    case MEMR_EPISODIC:   return "episodic";
    case MEMR_PROCEDURAL: return "procedural";
    case MEMR_PREFERENCE: return "preference";
    case MEMR_PROJECT:    return "project";
    case MEMR_ENTITY:     return "entity";
    }
    return "semantic";
}

static mem_type_t type_from(const char *s) {
    if (strcmp(s, "episodic") == 0)   return MEMR_EPISODIC;
    if (strcmp(s, "procedural") == 0) return MEMR_PROCEDURAL;
    if (strcmp(s, "preference") == 0) return MEMR_PREFERENCE;
    if (strcmp(s, "project") == 0)    return MEMR_PROJECT;
    if (strcmp(s, "entity") == 0)     return MEMR_ENTITY;
    return MEMR_SEMANTIC;
}

static const char *status_name(mem_status_t s) {
    switch (s) {
    case MEM_STATUS_PROVISIONAL:  return "provisional";
    case MEM_STATUS_TRUSTED:      return "trusted";
    case MEM_STATUS_VERIFIED:     return "verified";
    case MEM_STATUS_DEPRECATED:   return "deprecated";
    case MEM_STATUS_CONTRADICTED: return "contradicted";
    case MEM_STATUS_ARCHIVED:     return "archived";
    }
    return "provisional";
}

static mem_status_t status_from(const char *s) {
    if (strcmp(s, "trusted") == 0)      return MEM_STATUS_TRUSTED;
    if (strcmp(s, "verified") == 0)     return MEM_STATUS_VERIFIED;
    if (strcmp(s, "deprecated") == 0)   return MEM_STATUS_DEPRECATED;
    if (strcmp(s, "contradicted") == 0) return MEM_STATUS_CONTRADICTED;
    if (strcmp(s, "archived") == 0)     return MEM_STATUS_ARCHIVED;
    return MEM_STATUS_PROVISIONAL;
}

/* Records in an exit state are excluded from the derived index. */
static int status_indexable(mem_status_t s) {
    return s != MEM_STATUS_DEPRECATED && s != MEM_STATUS_CONTRADICTED &&
           s != MEM_STATUS_ARCHIVED;
}

static const char *outcome_name(mem_outcome_t o) {
    switch (o) {
    case MEM_OUTCOME_SUCCESS: return "SUCCESS";
    case MEM_OUTCOME_FAILURE: return "FAILURE";
    case MEM_OUTCOME_PARTIAL: return "PARTIAL_SUCCESS";
    case MEM_OUTCOME_UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

static mem_outcome_t outcome_from(const char *s) {
    if (strcmp(s, "SUCCESS") == 0)         return MEM_OUTCOME_SUCCESS;
    if (strcmp(s, "FAILURE") == 0)         return MEM_OUTCOME_FAILURE;
    if (strcmp(s, "PARTIAL_SUCCESS") == 0) return MEM_OUTCOME_PARTIAL;
    return MEM_OUTCOME_UNKNOWN;
}

/* --- (de)serialization -------------------------------------------------- */

const char *mem_record_type_name(mem_type_t t) {
    return type_name(t);
}

const char *mem_record_status_name(mem_status_t s) {
    return status_name(s);
}

char *mem_record_to_md(const mem_record *r) {
    if (!r || !r->id[0] || !r->content)
        return NULL;

    strbuf sb;
    strbuf_init(&sb);
    strbuf_appendf(&sb, "---\n");
    strbuf_appendf(&sb, "id: %s\n", r->id);
    strbuf_appendf(&sb, "type: %s\n", type_name(r->type));
    if (r->title[0])
        strbuf_appendf(&sb, "title: \"%s\"\n", r->title);
    strbuf_appendf(&sb, "scope: %s\n", r->scope[0] ? r->scope : "global");
    strbuf_appendf(&sb, "status: %s\n", status_name(r->status));
    strbuf_appendf(&sb, "importance: %.3f\n", r->importance);
    strbuf_appendf(&sb, "confidence: %.3f\n", r->confidence);
    strbuf_appendf(&sb, "utility: %.3f\n", r->utility);
    strbuf_appendf(&sb, "emotion:\n");
    strbuf_appendf(&sb, "  valence: %.3f\n", r->valence);
    strbuf_appendf(&sb, "  arousal: %.3f\n", r->arousal);
    strbuf_appendf(&sb, "  salience: %.3f\n", r->salience);
    strbuf_appendf(&sb, "source:\n");
    strbuf_appendf(&sb, "  type: %s\n", r->source[0] ? r->source : "experience");
    if (r->origin_id[0])
        strbuf_appendf(&sb, "  derived_from: %s\n", r->origin_id);
    strbuf_appendf(&sb, "outcome: %s\n", outcome_name(r->outcome));
    if (r->evidence[0])
        strbuf_appendf(&sb, "evidence: \"%s\"\n", r->evidence);
    strbuf_appendf(&sb, "version: %d\n", r->version);
    strbuf_appendf(&sb, "created: %lld\n", r->created_ms);
    strbuf_appendf(&sb, "updated: %lld\n", r->updated_ms);
    strbuf_appendf(&sb, "access_count: %u\n", r->access_count);
    strbuf_appendf(&sb, "last_access: %lld\n", r->last_access_ms);
    strbuf_appendf(&sb, "---\n");
    strbuf_append(&sb, r->content);
    if (r->content[0] && r->content[strlen(r->content) - 1] != '\n')
        strbuf_append(&sb, "\n");
    return strbuf_detach(&sb);
}

/* Copy val into dst, stripping the surrounding quotes the serializer writes. */
static void copy_quoted(char *dst, size_t n, const char *val) {
    size_t len = strlen(val);
    if (len >= 2 && val[0] == '"' && val[len - 1] == '"') {
        size_t inner = len - 2;
        if (inner >= n)
            inner = n - 1;
        memcpy(dst, val + 1, inner);
        dst[inner] = '\0';
    } else {
        snprintf(dst, n, "%s", val);
    }
}

/* Parse one "key: value" line into the record. Returns 1 handled. */
static int feed_field(mem_record *r, const char *key, const char *val, int in_emotion,
                      int in_source) {
    if (in_emotion) {
        if (strcmp(key, "valence") == 0) { r->valence = atof(val); return 1; }
        if (strcmp(key, "arousal") == 0) { r->arousal = atof(val); return 1; }
        if (strcmp(key, "salience") == 0) { r->salience = atof(val); return 1; }
        return 0;
    }
    if (in_source) {
        if (strcmp(key, "type") == 0) { snprintf(r->source, sizeof r->source, "%s", val); return 1; }
        if (strcmp(key, "derived_from") == 0) {
            snprintf(r->origin_id, sizeof r->origin_id, "%s", val);
            return 1;
        }
        return 0;
    }
    if (strcmp(key, "id") == 0)            { snprintf(r->id, sizeof r->id, "%s", val); return 1; }
    if (strcmp(key, "type") == 0)          { r->type = type_from(val); return 1; }
    if (strcmp(key, "title") == 0)         { copy_quoted(r->title, sizeof r->title, val); return 1; }
    if (strcmp(key, "scope") == 0)         { snprintf(r->scope, sizeof r->scope, "%s", val); return 1; }
    if (strcmp(key, "status") == 0)        { r->status = status_from(val); return 1; }
    if (strcmp(key, "importance") == 0)    { r->importance = atof(val); return 1; }
    if (strcmp(key, "confidence") == 0)    { r->confidence = atof(val); return 1; }
    if (strcmp(key, "utility") == 0)       { r->utility = atof(val); return 1; }
    if (strcmp(key, "outcome") == 0)       { r->outcome = outcome_from(val); return 1; }
    if (strcmp(key, "evidence") == 0)      { copy_quoted(r->evidence, sizeof r->evidence, val); return 1; }
    if (strcmp(key, "version") == 0)       { r->version = atoi(val); return 1; }
    if (strcmp(key, "created") == 0)       { r->created_ms = atoll(val); return 1; }
    if (strcmp(key, "updated") == 0)       { r->updated_ms = atoll(val); return 1; }
    if (strcmp(key, "access_count") == 0)  { r->access_count = (unsigned)atoi(val); return 1; }
    if (strcmp(key, "last_access") == 0)   { r->last_access_ms = atoll(val); return 1; }
    return 0;
}

mem_record *mem_record_from_md(const char *md) {
    if (!md || strncmp(md, "---\n", 4) != 0)
        return NULL;

    mem_record *r = (mem_record *)calloc(1, sizeof(mem_record));
    if (!r)
        return NULL;
    r->type = MEMR_SEMANTIC;
    r->status = MEM_STATUS_PROVISIONAL;
    r->outcome = MEM_OUTCOME_UNKNOWN;
    r->version = 1;

    const char *p = md + 4;
    int in_emotion = 0, in_source = 0;
    for (;;) {
        const char *nl = strchr(p, '\n');
        if (!nl)
            goto fail;               /* unterminated frontmatter */
        size_t len = (size_t)(nl - p);
        if (len == 3 && strncmp(p, "---", 3) == 0) {
            p = nl + 1;
            break;                   /* end of frontmatter */
        }
        /* copy the line (frontmatter lines are short) */
        char line[512];
        if (len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = '\0';

        if (line[0] == ' ' || line[0] == '\t') {
            /* nested field under emotion:/source: */
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                char *key = line;
                while (*key == ' ' || *key == '\t')
                    key++;
                char *val = colon + 1;
                while (*val == ' ')
                    val++;
                feed_field(r, key, val, in_emotion, in_source);
            }
        } else {
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                char *val = colon + 1;
                while (*val == ' ')
                    val++;
                in_emotion = strcmp(line, "emotion") == 0;
                in_source = strcmp(line, "source") == 0;
                if (!in_emotion && !in_source)
                    feed_field(r, line, val, 0, 0);
            }
        }
        p = nl + 1;
    }

    if (!r->id[0])
        goto fail;
    r->content = xstrdup(p);
    if (!r->content)
        goto fail;
    return r;

fail:
    mem_record_free(r);
    return NULL;
}

void mem_record_free(mem_record *r) {
    if (!r)
        return;
    free(r->content);
    free(r);
}

/* --- store --------------------------------------------------------------- */

struct mem_records {
    char dir[512];
    mem_record **items;
    size_t count, cap;
    mutex_t mtx;
};

static void records_path(const mem_records *rs, char *out, size_t n, const char *id) {
    char name[MEM_ID_MAX + 8];
    snprintf(name, sizeof name, "%s.md", id);
    path_join(out, n, rs->dir, name);
}

mem_records *mem_records_new(const char *root) {
    if (!root || !*root)
        return NULL;
    mem_records *rs = (mem_records *)calloc(1, sizeof(mem_records));
    if (!rs)
        return NULL;
    path_join(rs->dir, sizeof rs->dir, root, "records");
    mutex_init(&rs->mtx);
    fs_mkdirs(rs->dir);

    dir_list dl;
    if (fs_list_dir(rs->dir, &dl) == 0) {
        for (size_t i = 0; i < dl.count; i++) {
            size_t nl = strlen(dl.items[i].name);
            if (dl.items[i].is_dir || nl < 4 ||
                strcmp(dl.items[i].name + nl - 3, ".md") != 0)
                continue;
            char p[1024];
            path_join(p, sizeof p, rs->dir, dl.items[i].name);
            char *md = fs_read_file(p);
            if (!md)
                continue;
            mem_record *r = mem_record_from_md(md);
            free(md);
            if (!r)
                continue;
            if (rs->count == rs->cap) {
                size_t ncap = rs->cap ? rs->cap * 2 : 16;
                mem_record **ni = (mem_record **)realloc(rs->items, ncap * sizeof(mem_record *));
                if (!ni) {
                    mem_record_free(r);
                    continue;
                }
                rs->items = ni;
                rs->cap = ncap;
            }
            rs->items[rs->count++] = r;
        }
        fs_list_free(&dl);
    }
    return rs;
}

void mem_records_free(mem_records *rs) {
    if (!rs)
        return;
    for (size_t i = 0; i < rs->count; i++)
        mem_record_free(rs->items[i]);
    free(rs->items);
    mutex_destroy(&rs->mtx);
    free(rs);
}

static int mem_records_put_impl(mem_records *rs, const mem_record *r);

int mem_records_put(mem_records *rs, const mem_record *rin) {
    mem_record tmp;
    char *clean;
    int rc;
    const mem_record *r = rin;

    if (!rs || !rin || !rin->id[0] || !rin->content)
        return -1;
    /* Secret Security Plane (§12): the canonical memory store never holds
     * plaintext secrets — redact MEDIUM/HIGH matches before storage. */
    clean = secret_redact_text(rin->content, strlen(rin->content), NULL);
    if (clean) {
        tmp = *rin;
        tmp.content = clean;
        r = &tmp;
    }
    rc = mem_records_put_impl(rs, r);
    free(clean);
    return rc;
}

static int mem_records_put_impl(mem_records *rs, const mem_record *r) {
    if (!rs || !r || !r->id[0] || !r->content)
        return -1;

    /* replace-in-place keeps insertion order stable */
    for (size_t i = 0; i < rs->count; i++) {
        if (strcmp(rs->items[i]->id, r->id) == 0) {
            mem_record *nr = (mem_record *)calloc(1, sizeof(mem_record));
            if (!nr)
                return -1;
            *nr = *r;
            nr->content = xstrdup(r->content);
            if (!nr->content) {
                free(nr);
                return -1;
            }
            mem_record_free(rs->items[i]);
            rs->items[i] = nr;
            goto persist;
        }
    }
    if (rs->count == rs->cap) {
        size_t ncap = rs->cap ? rs->cap * 2 : 16;
        mem_record **ni = (mem_record **)realloc(rs->items, ncap * sizeof(mem_record *));
        if (!ni)
            return -1;
        rs->items = ni;
        rs->cap = ncap;
    }
    {
        mem_record *nr = (mem_record *)calloc(1, sizeof(mem_record));
        if (!nr)
            return -1;
        *nr = *r;
        nr->content = xstrdup(r->content);
        if (!nr->content) {
            free(nr);
            return -1;
        }
        rs->items[rs->count++] = nr;
    }
persist:;
    char *md = mem_record_to_md(r);
    if (!md)
        return -1;
    char p[1024];
    records_path(rs, p, sizeof p, r->id);
    int rc = fs_write_file(p, md, strlen(md));
    free(md);
    return rc == 0 ? 0 : -1;
}

const mem_record *mem_records_at(mem_records *rs, int i) {
    if (!rs || i < 0 || (size_t)i >= rs->count)
        return NULL;
    return rs->items[i];
}

int mem_records_count(mem_records *rs) {
    return rs ? (int)rs->count : 0;
}

const mem_record *mem_records_find(mem_records *rs, const char *id) {
    if (!rs || !id)
        return NULL;
    for (size_t i = 0; i < rs->count; i++)
        if (strcmp(rs->items[i]->id, id) == 0)
            return rs->items[i];
    return NULL;
}

const mem_record *mem_records_find_origin(mem_records *rs, const char *origin_id) {
    if (!rs || !origin_id || !*origin_id)
        return NULL;
    for (size_t i = 0; i < rs->count; i++)
        if (strcmp(rs->items[i]->origin_id, origin_id) == 0)
            return rs->items[i];
    return NULL;
}

int mem_records_remove(mem_records *rs, const char *id) {
    if (!rs || !id)
        return -1;
    for (size_t i = 0; i < rs->count; i++) {
        if (strcmp(rs->items[i]->id, id) == 0) {
            mem_record_free(rs->items[i]);
            memmove(&rs->items[i], &rs->items[i + 1],
                    (rs->count - i - 1) * sizeof(mem_record *));
            rs->count--;
            char p[1024];
            records_path(rs, p, sizeof p, id);
            fs_remove(p);
            return 1;
        }
    }
    return 0;
}

int mem_records_rebuild_index(mem_records *rs, void *v) {
    vectorstore *vs = (vectorstore *)v;
    if (!rs || !vs)
        return -1;
    /* wipe previous record mirrors (idempotent rebuild) */
    for (size_t i = 0; i < rs->count; i++) {
        char vid[64];
        snprintf(vid, sizeof vid, "r:%s", rs->items[i]->id);
        vectorstore_remove(vs, vid);
    }
    int n = 0;
    for (size_t i = 0; i < rs->count; i++) {
        const mem_record *r = rs->items[i];
        if (!status_indexable(r->status))
            continue;
        char vid[64];
        snprintf(vid, sizeof vid, "r:%s", r->id);
        vectorstore_add(vs, vid, r->content, "record");
        n++;
    }
    return n;
}

void mem_records_touch(mem_records *rs, const char *id, long long now_ms) {
    if (!rs || !id)
        return;
    for (size_t i = 0; i < rs->count; i++) {
        if (strcmp(rs->items[i]->id, id) == 0) {
            rs->items[i]->access_count++;
            if (now_ms > 0)
                rs->items[i]->last_access_ms = now_ms;
            return;
        }
    }
}

char *mem_records_json(mem_records *rs) {
    if (!rs)
        return NULL;
    strbuf sb;
    strbuf_init(&sb);
    strbuf_append(&sb, "[");
    for (size_t i = 0; i < rs->count; i++) {
        const mem_record *r = rs->items[i];
        if (i)
            strbuf_append(&sb, ",");
        strbuf_appendf(&sb, "{\"id\":\"%s\",\"type\":\"%s\",\"scope\":\"%s\","
                            "\"status\":\"%s\",\"importance\":%.3f,\"confidence\":%.3f,"
                            "\"utility\":%.3f,\"outcome\":\"%s\",\"version\":%d,"
                            "\"access_count\":%u}",
                       r->id, type_name(r->type), r->scope, status_name(r->status),
                       r->importance, r->confidence, r->utility, outcome_name(r->outcome),
                       r->version, r->access_count);
    }
    strbuf_append(&sb, "]");
    return strbuf_detach(&sb);
}
