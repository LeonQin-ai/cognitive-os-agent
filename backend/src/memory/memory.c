/* memory.c — cognitive memory facade.
 * Composes fine-grained sub-stores:
 *  - working memory:  short-term ring buffer of recent items (inline)
 *  - long-term facts: kvstore (memory/kv.h)
 *  - episodes:        episodic (memory/episode.h)
 *  - vector-lite:     vectorstore (memory/vector.h), mirroring working + episodes
 * Long-term facts are persisted as JSON under the state root. */
#include "cognitive-os-agent/memory/memory.h"
#include "cognitive-os-agent/memory/kv.h"
#include "cognitive-os-agent/memory/episode.h"
#include "cognitive-os-agent/memory/vector.h"
#include "cognitive-os-agent/memory/graph.h"
#include "cognitive-os-agent/retrieval/embedding.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"

#define WORKING_CAP 64

/* shared keyword tokenizer (defined below; used by search + graph recall) */
static void tokenize(const char *s, const char *tokens[64], int tlens[64], int *ntok);
static int token_match(const char *text, const char *tok, size_t tlen);

typedef struct {
    char **items; /* newest first */
    char **ids;   /* parallel vector-store ids ("w:<seq>"), newest first */
    size_t count;
} working_mem;

struct memory {
    char root[512];
    working_mem working;      /* guarded by mtx */
    kvstore *facts;       /* own mutex */
    episodic *episodes;   /* own mutex */
    vectorstore *vectors; /* own mutex */
    graph *graph;         /* entity graph: task -used-> tool -touched-> file */
    size_t seq;               /* monotonic id for vector mirroring (guarded by mtx) */
    /* automatic consolidation bookkeeping (guarded by mtx) */
    size_t consolidated_at; /* episode count at the last pass */
    long long last_consol_ms;
    int consol_count;
    /* automatic lifecycle pass config (guarded by mtx) */
    long long lc_half_life_ms; /* <= 0 = auto lifecycle off */
    double lc_min_strength;
    int lc_archive;
    mutex_t mtx;
};

/* Per-instance persistence paths: memory files live under
 * <state_root>/memory/. With no state_root the instance is ephemeral
 * (in-memory only) -- this keeps contexts and tests isolated instead of
 * sharing a single global directory. */
static void mem_path(const memory *m, char *out, size_t n, const char *file) {
    if (m->root[0] && file && *file) {
        path_join(out, n, m->root, "memory");
        path_join(out, n, out, file);
        return;
    }

    out[0] = 0;
}

static char *mem_read(const memory *m, const char *file) {
    char p[1024];
    mem_path(m, p, sizeof p, file);
    return p[0] ? fs_read_file(p) : NULL;
}

static int mem_write(const memory *m, const char *file, const char *text) {
    char p[1024];
    char dir[1024];

    mem_path(m, p, sizeof p, file);
    if (!p[0])
        return -1;
    path_join(dir, sizeof dir, m->root, "memory");
    fs_mkdirs(dir);
    return fs_write_file(p, text, strlen(text));
}

memory *memory_new(const char *state_root) {
    memory *m = (memory *)calloc(1, sizeof(memory));
    /* load persisted facts */
    char *facts_json;
    /* load persisted episodes (experience survives restarts) */
    char *ep_json;
    /* load persisted entity graph */
    char *g_json;

    if (!m)
        return NULL;
    snprintf(m->root, sizeof(m->root), "%s", state_root);
    mutex_init(&m->mtx);
    m->facts = kvstore_new();
    m->episodes = episodic_new();
    m->vectors = vectorstore_new();
    m->graph = graph_new();
    if (!m->facts || !m->episodes || !m->vectors || !m->graph) {
        if (m->facts)
            kvstore_free(m->facts);
        if (m->episodes)
            episodic_free(m->episodes);
        if (m->vectors)
            vectorstore_free(m->vectors);
        if (m->graph)
            graph_free(m->graph);
        mutex_destroy(&m->mtx);
        free(m);
        return NULL;
    }

    /* load persisted facts */
    facts_json = mem_read(m, "facts.json");
    if (facts_json) {
        cJSON *root = cJSON_Parse(facts_json);
        if (root && cJSON_IsObject(root)) {
            cJSON *it;
            cJSON_ArrayForEach(it, root) kvstore_set(m->facts, it->string, it->valuestring ? it->valuestring : "");
        }
        if (root)
            cJSON_Delete(root);
        free(facts_json);
    }

    /* load persisted episodes (experience survives restarts) */
    ep_json = mem_read(m, "episodes.json");
    if (ep_json) {
        cJSON *root = cJSON_Parse(ep_json);
        if (root && cJSON_IsArray(root)) {
            cJSON *it;
            cJSON_ArrayForEach(it, root) {
                cJSON *tk = cJSON_GetObjectItemCaseSensitive(it, "task");
                cJSON *rs = cJSON_GetObjectItemCaseSensitive(it, "result");
                cJSON *ts = cJSON_GetObjectItemCaseSensitive(it, "ts");
                cJSON *st = cJSON_GetObjectItemCaseSensitive(it, "strength");
                if (tk && cJSON_IsString(tk))
                    episodic_add_full(m->episodes, tk->valuestring,
                                          (rs && cJSON_IsString(rs)) ? rs->valuestring : "",
                                          (ts && cJSON_IsNumber(ts)) ? (long long)ts->valuedouble : 0,
                                          (st && cJSON_IsNumber(st)) ? st->valuedouble : 0);
            }
        }
        if (root)
            cJSON_Delete(root);
        free(ep_json);
    }

    /* mirror reloaded episodes into the vector store so RAG recall
     * (memory_retrieve/_ex, used by the context builder) still finds
     * pre-restart experience — keyword search alone saw them before, which
     * made the agent "forget" after a restart. Rebuild ids from the shared
     * sequence so newly recorded episodes cannot collide. */
    {
        int n = episodic_count(m->episodes);
        mutex_lock(&m->mtx);
        for (int i = 0; i < n; i++) {
            const char *t = episodic_task(m->episodes, i);
            if (!t || !*t)
                continue;
            char id[32];
            snprintf(id, sizeof(id), "e:%zu", m->seq++);
            vectorstore_add(m->vectors, id, t, "episode");
        }
        mutex_unlock(&m->mtx);
    }

    /* load persisted entity graph */
    g_json = mem_read(m, "graph.json");
    if (g_json) {
        cJSON *root = cJSON_Parse(g_json);
        cJSON *edges = root ? cJSON_GetObjectItemCaseSensitive(root, "edges") : NULL;
        if (edges && cJSON_IsArray(edges)) {
            cJSON *it;
            cJSON_ArrayForEach(it, edges) {
                cJSON *f = cJSON_GetObjectItemCaseSensitive(it, "from");
                cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "to");
                cJSON *r = cJSON_GetObjectItemCaseSensitive(it, "relation");
                if (f && cJSON_IsString(f) && t && cJSON_IsString(t)) {
                    graph_add_node(m->graph, f->valuestring, f->valuestring);
                    graph_add_node(m->graph, t->valuestring, t->valuestring);
                    graph_add_edge(m->graph, f->valuestring, t->valuestring,
                                       (r && cJSON_IsString(r)) ? r->valuestring : "");
                }
            }
        }
        if (root)
            cJSON_Delete(root);
        free(g_json);
    }

    return m;
}

void memory_free(memory *m) {
    if (!m)
        return;
    for (size_t i = 0; i < m->working.count; i++) {
        free(m->working.items[i]);
        if (m->working.ids)
            free(m->working.ids[i]);
    }

    free(m->working.items);
    free(m->working.ids);
    kvstore_free(m->facts);
    episodic_free(m->episodes);
    vectorstore_free(m->vectors);
    graph_free(m->graph);
    mutex_destroy(&m->mtx);
    free(m);
}

void memory_working_push(memory *m, const char *text) {
    char id[32];
    char *evict_id = NULL;
    char **ni;

    if (!m || !text)
        return;
     /* vector-store id of the evicted ring item */
    mutex_lock(&m->mtx);
    /* id list mirrors the ring 1:1 (ring never exceeds WORKING_CAP, so one
     * lazy allocation is enough); NULL entries mean "not tracked" (OOM). */
    if (!m->working.ids)
        m->working.ids = (char **)calloc(WORKING_CAP, sizeof(char *));
    if (m->working.count == WORKING_CAP) {
        free(m->working.items[WORKING_CAP - 1]);
        if (m->working.ids) {
            evict_id = m->working.ids[WORKING_CAP - 1];
            m->working.ids[WORKING_CAP - 1] = NULL;
        }
        m->working.count--;
    }

    ni = (char **)realloc(m->working.items, (m->working.count + 1) * sizeof(char *));
    if (!ni) {
        mutex_unlock(&m->mtx);
        free(evict_id);
        return;
    }

    m->working.items = ni;
    memmove(m->working.items + 1, m->working.items, m->working.count * sizeof(char *));
    m->working.items[0] = xstrdup(text);
    m->working.count++;
    snprintf(id, sizeof(id), "w:%zu", m->seq++);
    if (m->working.ids) {
        memmove(m->working.ids + 1, m->working.ids, (m->working.count - 1) * sizeof(char *));
        m->working.ids[0] = xstrdup(id);
    }

    mutex_unlock(&m->mtx);

    vectorstore_add(m->vectors, id, text, "working");
    if (evict_id) {
        /* the ring evicted the oldest item: drop its vector mirror too,
         * otherwise the store grows without bound in long sessions */
        vectorstore_remove(m->vectors, evict_id);
        free(evict_id);
    }
}

int memory_working_count(memory *m) {
    int n;

    if (!m)
        return 0;
    mutex_lock(&m->mtx);
    n = (int)m->working.count;
    mutex_unlock(&m->mtx);
    return n;
}

const char *memory_working_at(memory *m, int i) {
    if (!m || i < 0)
        return NULL;
    mutex_lock(&m->mtx);
    const char *v = ((size_t)i < m->working.count) ? m->working.items[i] : NULL;
    mutex_unlock(&m->mtx);
    return v;
}

void memory_remember(memory *m, const char *key, const char *value) {
    if (!m || !key || !*key)
        return;
    kvstore_set(m->facts, key, value);
}

const char *memory_recall(memory *m, const char *key) {
    if (!m || !key)
        return NULL;
    return kvstore_get(m->facts, key);
}

void memory_record_experience(memory *m, const char *task, const char *result) {
    char id[32];

    if (!m || !task)
        return;
    mutex_lock(&m->mtx);
    snprintf(id, sizeof(id), "e:%zu", m->seq++);
    mutex_unlock(&m->mtx);

    episodic_add(m->episodes, task, result);
    vectorstore_add(m->vectors, id, task, result ? result : "");
}

/* ---- RAG document indexing (uploads) ---- */

int memory_index_document(memory *m, const char *id, const char *text, const char *meta) {
    if (!m || !text || !*text)
        return -1;
    return vectorstore_add(m->vectors, id, text, meta);
}

#define UPLOAD_CHUNK_TARGET 600             /* chunk chars before flushing a paragraph run */
#define UPLOAD_MAX_FILE (8LL * 1024 * 1024) /* skip files above 8MB */

/* Split `text` into ~UPLOAD_CHUNK_TARGET chunks at paragraph boundaries
 * (blank lines) and index each with id "<base>#<i>". Returns chunks added. */
int memory_index_text(memory *m, const char *base, const char *text) {
    strbuf cur;
    const char *p = text;

    strbuf_init(&cur);
    int n = 0, idx = 0;
    while (*p) {
        /* one paragraph: up to a blank line or EOF */
        const char *nl = strstr(p, "\n\n");
        size_t plen = nl ? (size_t)(nl - p) : strlen(p);
        if (cur.len > 0 && cur.len + plen > UPLOAD_CHUNK_TARGET) {
            char id[128];
            snprintf(id, sizeof(id), "%s#%d", base, idx++);
            memory_index_document(m, id, cur.buf, "upload");
            n++;
            strbuf_free(&cur);
            strbuf_init(&cur);
        }
        strbuf_append_n(&cur, p, plen);
        strbuf_append(&cur, "\n");
        p += nl ? (size_t)(nl - p) + 2 : plen;
    }

    if (cur.len > 0) {
        char id[128];
        snprintf(id, sizeof(id), "%s#%d", base, idx++);
        memory_index_document(m, id, cur.buf, "upload");
        n++;
    }

    strbuf_free(&cur);
    return n;
}

int memory_index_uploads(memory *m, const char *dir) {
    dir_list dl;
    int total = 0;

    if (!m || !dir || !*dir)
        return 0;
    if (fs_list_dir(dir, &dl) != 0)
        return 0;
    for (size_t i = 0; i < dl.count; i++) {
        if (dl.items[i].is_dir)
            continue;
        char fpath[1024];
        path_join(fpath, sizeof(fpath), dir, dl.items[i].name);
        long long sz = fs_file_size(fpath);
        if (sz <= 0 || sz > UPLOAD_MAX_FILE)
            continue;
        char *text = fs_read_file(fpath);
        if (!text)
            continue;
        char base[256];
        snprintf(base, sizeof(base), "upload:%s", dl.items[i].name);
        total += memory_index_text(m, base, text);
        free(text);
    }

    fs_list_free(&dl);
    return total;
}

/* ---- entity knowledge graph (task -used-> tool -touched-> file) ---- */

void memory_record_edge(memory *m, const char *from, const char *to, const char *relation) {
    if (!m || !from || !*from || !to || !*to)
        return;
    /* nodes are id = label; existing nodes are folded (add_node returns -1) */
    graph_add_node(m->graph, from, from);
    graph_add_node(m->graph, to, to);
    graph_add_edge(m->graph, from, to, relation ? relation : "");
}

char *memory_graph_json(memory *m) {
    if (!m)
        return xstrdup("{}");
    return graph_snapshot_json(m->graph);
}

/* Edges whose endpoint labels share a token with the query. Reuses the
 * episode tokenizer: tokens < 3 chars are ignored, matching is case-insensitive. */
static int label_hit(const char *label, const char **tokens, const int *tlens, int ntok) {
    if (!label)
        return 0;
    for (int t = 0; t < ntok; t++)
        if (token_match(label, tokens[t], (size_t)tlens[t]))
            return 1;
    return 0;
}

char *memory_graph_related(memory *m, const char *query, int limit) {
    const char *tokens[64];
    int tlens[64];
    int ntok = 0;
    char *snap;
    cJSON *root;
    cJSON *arr;
    cJSON *edges;
    char *s;

    if (!m || !query || limit <= 0)
        return xstrdup("[]");
    tokenize(query, tokens, tlens, &ntok);
    if (ntok == 0)
        return xstrdup("[]");

    snap = graph_snapshot_json(m->graph);
    root = snap ? cJSON_Parse(snap) : NULL;
    free(snap);
    arr = cJSON_CreateArray();
    edges = root ? cJSON_GetObjectItemCaseSensitive(root, "edges") : NULL;
    if (arr && edges && cJSON_IsArray(edges)) {
        cJSON *it;
        cJSON_ArrayForEach(it, edges) {
    cJSON *f;
    cJSON *t;
    cJSON *r;
    char text[700];
    cJSON *o;

            if (cJSON_GetArraySize(arr) >= limit)
                break;
            f = cJSON_GetObjectItemCaseSensitive(it, "from");
            t = cJSON_GetObjectItemCaseSensitive(it, "to");
            r = cJSON_GetObjectItemCaseSensitive(it, "relation");
            const char *fs = f && cJSON_IsString(f) ? f->valuestring : NULL;
            const char *ts = t && cJSON_IsString(t) ? t->valuestring : NULL;
            const char *rs = r && cJSON_IsString(r) ? r->valuestring : "";
            if (!fs || !ts)
                continue;
            if (!label_hit(fs, tokens, tlens, ntok) && !label_hit(ts, tokens, tlens, ntok))
                continue;
            snprintf(text, sizeof(text), "%s -%s-> %s", fs, rs, ts);
            o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "text", text);
            cJSON_AddItemToArray(arr, o);
        }
    }

    if (root)
        cJSON_Delete(root);
    s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr)
        cJSON_Delete(arr);
    return s ? s : xstrdup("[]");
}

/* ---- consolidation engine: recurring episode themes -> long-term facts ---- */

int memory_consolidate(memory *m) {
    size_t n_acc = 0;
    int n;
    int written = 0;

    if (!m)
        return 0;
    /* token -> how many distinct episodes contain it (episodes are oldest
     * first, so a single backwards pass per token keeps the last seen index) */
    typedef struct {
        char tok[64];
        int eps;
        size_t last;
    } topic;
    topic acc[128];

    n = episodic_count(m->episodes);
    for (int i = 0; i < n; i++) {
        const char *task = episodic_task(m->episodes, i);
        if (!task)
            continue;
        const char *tokens[64];
        int tlens[64];
        int ntok = 0;
        tokenize(task, tokens, tlens, &ntok);
        for (int t = 0; t < ntok; t++) {
            size_t len = (size_t)tlens[t];
            if (len >= sizeof(acc[0].tok))
                continue;
            /* skip pure numbers */
            int digit_only = 1;
            for (size_t c = 0; c < len; c++)
                if (!isdigit((unsigned char)tokens[t][c])) {
                    digit_only = 0;
                    break;
                }
            if (digit_only)
                continue;
            char tok[64];
            memcpy(tok, tokens[t], len);
            tok[len] = '\0';
            topic *found = NULL;
            for (size_t a = 0; a < n_acc; a++)
                if (strcmp(acc[a].tok, tok) == 0) {
                    found = &acc[a];
                    break;
                }
            if (!found) {
                if (n_acc >= sizeof(acc) / sizeof(acc[0]))
                    continue;
                found = &acc[n_acc++];
                memset(found, 0, sizeof(*found));
                memcpy(found->tok, tok, len + 1);
                found->eps = 0;
                found->last = (size_t)-1;
            }
            if (found->last != (size_t)i) {
                found->eps++;
                found->last = (size_t)i;
            }
        }
    }

    for (size_t a = 0; a < n_acc; a++) {
        if (acc[a].eps < 3)
            continue; /* recurring theme threshold */
        char val[64];
        snprintf(val, sizeof(val), "seen in %d tasks", acc[a].eps);
        char key[80];
        snprintf(key, sizeof(key), "topic.%s", acc[a].tok);
        kvstore_set(m->facts, key, val);
        written++;
    }

    return written;
}

int memory_consolidation_count(memory *m) {
    int n;

    if (!m)
        return 0;
    mutex_lock(&m->mtx);
    n = m->consol_count;
    mutex_unlock(&m->mtx);
    return n;
}

/* Procedural distillation: count used_tool edges per tool from the graph
 * snapshot; tools recurring in >= min tasks become procedure.* facts. */
static int consolidate_procedural(memory *m, int min_tasks) {
    char *snap = graph_snapshot_json(m->graph);
    cJSON *root = snap ? cJSON_Parse(snap) : NULL;
    cJSON *edges;
    size_t n_seen = 0;
    cJSON *it;
    int written = 0;

    free(snap);
    if (!root)
        return 0;
    edges = cJSON_GetObjectItemCaseSensitive(root, "edges");
    if (!edges || !cJSON_IsArray(edges)) {
        cJSON_Delete(root);
        return 0;
    }

    typedef struct {
        const char *tool;
        int n;
    } proc;
    proc seen[64];
    cJSON_ArrayForEach(it, edges) {
        cJSON *rel = cJSON_GetObjectItemCaseSensitive(it, "relation");
        cJSON *to = cJSON_GetObjectItemCaseSensitive(it, "to");
    size_t a = 0;

        if (!rel || !cJSON_IsString(rel) || strcmp(rel->valuestring, "used_tool") != 0 || !to || !cJSON_IsString(to) ||
            !to->valuestring)
            continue;
        for (; a < n_seen; a++)
            if (strcmp(seen[a].tool, to->valuestring) == 0) {
                seen[a].n++;
                break;
            }

        if (a == n_seen && n_seen < sizeof(seen) / sizeof(seen[0])) {
            seen[n_seen].tool = to->valuestring;
            seen[n_seen].n = 1;
            n_seen++;
        }
    }

    for (size_t a = 0; a < n_seen; a++) {
        if (seen[a].n < min_tasks)
            continue;
        char key[96], val[64];
        snprintf(key, sizeof(key), "procedure.%s", seen[a].tool);
        snprintf(val, sizeof(val), "used in %d tasks", seen[a].n);
        kvstore_set(m->facts, key, val);
        written++;
    }

    cJSON_Delete(root);
    return written;
}

int memory_maybe_consolidate(memory *m, int threshold_eps, long long interval_ms) {
    int n;
    double ms;
    int arc;

    if (!m || threshold_eps <= 0)
        return -1;
    long long now = time_now_ms();
    mutex_lock(&m->mtx);
    if (m->last_consol_ms && interval_ms > 0 && now - m->last_consol_ms < interval_ms) {
        mutex_unlock(&m->mtx);
        return 0;
    }

    n = episodic_count(m->episodes);
    if (n - (int)m->consolidated_at < threshold_eps) {
        mutex_unlock(&m->mtx);
        return 0;
    }

    m->last_consol_ms = now;
    m->consolidated_at = (size_t)n;
    m->consol_count++;
    mutex_unlock(&m->mtx);

    /* distillation itself runs on the sub-stores' own locks */
    memory_consolidate(m);    /* semantic: recurring themes */
    consolidate_procedural(m, 2); /* procedural: recurring tools */
    /* automatic lifecycle pass rides along with consolidation (decay+forget) */
    mutex_lock(&m->mtx);
    long long hl = m->lc_half_life_ms;
    ms = m->lc_min_strength;
    arc = m->lc_archive;
    mutex_unlock(&m->mtx);
    if (hl > 0 || ms > 0) {
        memory_lifecycle_cfg lc = {0};
        lc.now_ms = now;
        lc.half_life_ms = hl;
        lc.min_strength = ms;
        lc.archive = arc;
        memory_lifecycle_pass(m, &lc);
    }

    return 1;
}

/* ---------- memory lifecycle: reinforce / decay / forget / archive ---------- */

void memory_set_lifecycle(memory *m, long long half_life_ms, double min_strength, int archive) {
    if (!m)
        return;
    mutex_lock(&m->mtx);
    m->lc_half_life_ms = half_life_ms;
    m->lc_min_strength = min_strength;
    m->lc_archive = archive;
    mutex_unlock(&m->mtx);
}

void memory_reinforce(memory *m, const char *task) {
    if (m)
        episodic_reinforce(m->episodes, task);
}

int memory_episode_count(memory *m) {
    return m ? episodic_count(m->episodes) : 0;
}

int memory_lifecycle_pass(memory *m, const memory_lifecycle_cfg *cfg) {
    double ms;
    int dropped = 0;

    if (!m)
        return -1;
    long long now = (cfg && cfg->now_ms > 0) ? cfg->now_ms : time_now_ms();
    long long hl = cfg ? cfg->half_life_ms : 0;
    ms = cfg ? cfg->min_strength : 0;
    if (hl > 0)
        episodic_decay(m->episodes, now, hl, 0.001);
    if (ms <= 0)
        return 0;
    if (cfg && cfg->archive) {
        char *below = episodic_below_json(m->episodes, ms);
        if (below && strcmp(below, "[]") != 0) {
            char p[1024];
            mem_path(m, p, sizeof p, "archive.jsonl");
            if (p[0]) {
                char dir[1024];
                path_join(dir, sizeof dir, m->root, "memory");
                fs_mkdirs(dir);
                char *line = (char *)malloc(strlen(below) + 2);
                if (line) {
                    snprintf(line, strlen(below) + 2, "%s\n", below);
                    fs_append_file(p, line, strlen(line));
                    free(line);
                }
            }
        }
        free(below);
    }

    dropped = episodic_drop_below(m->episodes, ms);
    return dropped;
}

static void tokenize(const char *s, const char *tokens[64], int tlens[64], int *ntok) {
    int n = 0;
    const char *p = s;
    while (*p && n < 64) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p))
            p++;
        size_t len = (size_t)(p - start);
        if (len >= 3) {
            tokens[n] = start; /* pointer into s; length in tlens (the string
                                  keeps going, so strlen() would be wrong) */
            tlens[n] = (int)len;
            n++;
        }
    }

    *ntok = n;
}

/* Portable case-insensitive compare of the first n bytes. */
static int ci_eq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca += 32;
        if (cb >= 'A' && cb <= 'Z')
            cb += 32;
        if (ca != cb)
            return 0;
    }

    return 1;
}

static int token_match(const char *text, const char *tok, size_t tlen) {
    const char *p = text;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && isalnum((unsigned char)*p))
            p++;
        if ((size_t)(p - s) == tlen && ci_eq_n(s, tok, tlen))
            return 1;
    }

    return 0;
}

char *memory_search(memory *m, const char *query, int limit) {
    const char *tokens[64];
    int tlens[64];
    int ntok = 0;
    cJSON *arr;
    char *s;

    tokenize(query, tokens, tlens, &ntok);
    if (ntok == 0)
        return xstrdup("[]");

    arr = cJSON_CreateArray();
    if (!arr)
        return xstrdup("[]");

    /* score working memory */
    mutex_lock(&m->mtx);
    for (size_t i = 0; i < m->working.count; i++) {
        int hits = 0;
        for (int t = 0; t < ntok; t++)
            if (token_match(m->working.items[i], tokens[t], (size_t)tlens[t]))
                hits++;
        if (hits > 0) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "kind", "working");
            cJSON_AddStringToObject(o, "text", m->working.items[i]);
            cJSON_AddNumberToObject(o, "score", hits);
            cJSON_AddItemToArray(arr, o);
            if (limit > 0 && cJSON_GetArraySize(arr) >= limit)
                break;
        }
    }

    mutex_unlock(&m->mtx);

    /* score experiences */
    if (limit <= 0 || cJSON_GetArraySize(arr) < limit) {
        int n = episodic_count(m->episodes);
        for (int i = 0; i < n; i++) {
            const char *task = episodic_task(m->episodes, i);
            const char *result = episodic_result(m->episodes, i);
            int hits = 0;
            for (int t = 0; t < ntok; t++)
                if (task && token_match(task, tokens[t], (size_t)tlens[t]))
                    hits++;
            if (hits > 0) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "kind", "experience");
                cJSON_AddStringToObject(o, "text", task ? task : "");
                cJSON_AddStringToObject(o, "result", result ? result : "");
                cJSON_AddNumberToObject(o, "ts", (double)episodic_ts(m->episodes, i));
                cJSON_AddNumberToObject(o, "score", hits);
                cJSON_AddItemToArray(arr, o);
                if (limit > 0 && cJSON_GetArraySize(arr) >= limit)
                    break;
            }
        }
    }

    s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : xstrdup("[]");
}

char *memory_retrieve(memory *m, const char *query, int k) {
    if (!m || !query)
        return xstrdup("[]");
    return vectorstore_nearest(m->vectors, query, k);
}

char *memory_retrieve_ex(memory *m, const char *query, int k, float w_vec) {
    /* stage 1: hybrid recall of an oversized candidate pool */
    char *cand_json;
    cJSON *cand;
    int nc;
    float *rel;
    float *recall;
    int i = 0;
    cJSON *it;
    /* order candidate indices by final score, take top k */
    int *order;
    cJSON *out;
    int kmax;
    char *s;

    if (!m || !query || k <= 0)
        return xstrdup("[]");
    if (w_vec < 0)
        w_vec = 0;
    if (w_vec > 1)
        w_vec = 1;
    /* stage 1: hybrid recall of an oversized candidate pool */
    cand_json = vectorstore_nearest_hybrid(m->vectors, query, k * 3, w_vec);
    if (!cand_json)
        return xstrdup("[]");
    cand = cJSON_Parse(cand_json);
    free(cand_json);
    if (!cand || !cJSON_IsArray(cand)) {
        cJSON_Delete(cand);
        return xstrdup("[]");
    }

    nc = cJSON_GetArraySize(cand);
    if (nc == 0) {
        cJSON_Delete(cand);
        return xstrdup("[]");
    }

    /* stage 2: rerank the candidates, blend with the recall score */
    const char **docs = (const char **)calloc((size_t)nc, sizeof(char *));
    rel = (float *)calloc((size_t)nc, sizeof(float));
    recall = (float *)calloc((size_t)nc, sizeof(float));
    if (!docs || !rel || !recall) {
        free(docs);
        free(rel);
        free(recall);
        cJSON_Delete(cand);
        return xstrdup("[]");
    }

    cJSON_ArrayForEach(it, cand) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "text");
        cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "score");
        docs[i] = (t && cJSON_IsString(t)) ? t->valuestring : "";
        recall[i] = (s && cJSON_IsNumber(s)) ? (float)s->valuedouble : 0.0f;
        i++;
    }

    embed_rerank(query, docs, (size_t)nc, rel);
    /* final = 0.6*rerank + 0.4*recall(hybrid) — rerank dominates but a strong
     * vector match survives a weak keyword overlap */
    for (int j = 0; j < nc; j++)
        rel[j] = 0.6f * rel[j] + 0.4f * recall[j];

    /* order candidate indices by final score, take top k */
    order = (int *)malloc((size_t)nc * sizeof(int));
    if (!order) {
        free(docs);
        free(rel);
        free(recall);
        cJSON_Delete(cand);
        return xstrdup("[]");
    }

    for (int j = 0; j < nc; j++)
        order[j] = j;
    for (int a = 1; a < nc; a++) {
        int idx = order[a];
        float v = rel[idx];
        int b = a - 1;
        while (b >= 0 && rel[order[b]] < v) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = idx;
    }

    out = cJSON_CreateArray();
    kmax = k < nc ? k : nc;
    for (int j = 0; j < kmax && out; j++) {
        cJSON *src = cJSON_GetArrayItem(cand, order[j]);
        cJSON *dup = cJSON_Duplicate(src, 1);
        if (dup)
            cJSON_AddItemToArray(out, dup);
    }

    free(order);
    free(docs);
    free(rel);
    free(recall);
    cJSON_Delete(cand);
    s = out ? cJSON_PrintUnformatted(out) : NULL;
    if (out)
        cJSON_Delete(out);
    return s ? s : xstrdup("[]");
}

char *memory_retrieve_mqe(memory *m, const char *const *queries, int nq, int k) {
    if (!m)
        return xstrdup("[]");
    return vectorstore_nearest_multi(m->vectors, queries, nq, k);
}

void memory_flush(memory *m) {
    char *s;
    char *e;
    char *g;

    if (!m)
        return;
    s = kvstore_snapshot_json(m->facts);
    if (s) {
        mem_write(m, "facts.json", s);
        free(s);
    }

    e = episodic_json(m->episodes);
    if (e) {
        mem_write(m, "episodes.json", e);
        free(e);
    }

    g = graph_snapshot_json(m->graph);
    if (g) {
        mem_write(m, "graph.json", g);
        free(g);
    }
}

char *memory_working_json(memory *m) {
    cJSON *arr;
    char *s;

    if (!m)
        return xstrdup("[]");
    mutex_lock(&m->mtx);
    arr = cJSON_CreateArray();
    if (arr)
        for (size_t i = 0; i < m->working.count; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(m->working.items[i]));
    mutex_unlock(&m->mtx);
    s = arr ? cJSON_PrintUnformatted(arr) : NULL;
    if (arr)
        cJSON_Delete(arr);
    return s ? s : xstrdup("[]");
}

char *memory_longterm_json(memory *m) {
    if (!m)
        return xstrdup("{}");
    return kvstore_snapshot_json(m->facts);
}

char *memory_episodes_json(memory *m) {
    if (!m)
        return xstrdup("[]");
    return episodic_json(m->episodes);
}
