/* context_builder.c — assemble retrieval context from memory.
 * Everything that enters the LLM prompt passes through here, so sizes are
 * capped per item and for the whole section, and stored episodes carry an
 * age annotation (stale memories are flagged, not asserted as fact). */
#include "cognitive-os-agent/retrieval/context_builder.h"
#include "cognitive-os-agent/memory/memory.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define CTX_ITEM_CAP 300   /* max chars per item text/result */
#define CTX_TOTAL_CAP 4096 /* max chars of the rendered context section */
#define CTX_FACTS_MAX 20   /* max long-term facts injected per query */

/* Append s to sb, truncating at a UTF-8 boundary and marking the cut. */
static void append_capped(strbuf *sb, const char *s, size_t cap) {
    size_t n;
    int trunc;

    if (!s)
        return;
    n = strlen(s);
    trunc = n > cap;
    if (trunc)
        n = cap;
    while (trunc && n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    strbuf_append_n(sb, s, n);
    if (trunc)
        strbuf_append(sb, "…");
}

/* Append {kind,text,result,score,ts} if `text` is not already present. */
static int append_unique(cJSON *arr, const char *kind, const char *text, const char *result, double score,
                         long long ts) {
    cJSON *it;
    cJSON *o;

    if (!arr || !text)
        return 0;
    cJSON_ArrayForEach(it, arr) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "text");
        if (t && cJSON_IsString(t) && strcmp(t->valuestring, text) == 0)
            return 0;
    }

    o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "kind", kind);
    cJSON_AddStringToObject(o, "text", text);
    cJSON_AddStringToObject(o, "result", result ? result : "");
    cJSON_AddNumberToObject(o, "score", score);
    if (ts > 0)
        cJSON_AddNumberToObject(o, "ts", (double)ts);
    cJSON_AddItemToArray(arr, o);
    return 1;
}

char *context_build(memory *m, const char *query, int max_items) {
    char *search;
    /* two-stage retrieval: hybrid recall -> rerank -> blended top-k */
    char *retr;
    cJSON *arr;
    int cap = max_items;
    cJSON *root;
    char *s;

    if (max_items <= 0)
        max_items = 8;
    search = m ? memory_search(m, query ? query : "", max_items) : xstrdup("[]");
    /* two-stage retrieval: hybrid recall -> rerank -> blended top-k */
    retr = m ? memory_retrieve_ex(m, query ? query : "", max_items, 0.7f) : xstrdup("[]");

    arr = cJSON_CreateArray();
    if (!arr) {
        free(search);
        free(retr);
        return xstrdup("[]");
    }

    /* long-term facts first (always relevant, small, explicitly stored) */
    if (m) {
        char *facts = memory_longterm_json(m);
        cJSON *root = cJSON_Parse(facts);
        if (root && cJSON_IsObject(root)) {
            int added = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, root) {
                if (added >= CTX_FACTS_MAX)
                    break;
                if (it->string) {
                    char kv[600];
                    snprintf(kv, sizeof(kv), "%s: %s", it->string,
                             (it->valuestring && cJSON_IsString(it)) ? it->valuestring : "");
                    if (append_unique(arr, "fact", kv, NULL, 1000.0, 0))
                        added++;
                }
            }
        }
        if (root)
            cJSON_Delete(root);
        free(facts);
    }

    root = cJSON_Parse(search);
    if (root && cJSON_IsArray(root)) {
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
    cJSON *kind;
    cJSON *text;
    cJSON *result;
    cJSON *score;
    cJSON *ts;

            if (cap <= 0 || cJSON_GetArraySize(arr) >= cap)
                break;
            kind = cJSON_GetObjectItemCaseSensitive(it, "kind");
            text = cJSON_GetObjectItemCaseSensitive(it, "text");
            result = cJSON_GetObjectItemCaseSensitive(it, "result");
            score = cJSON_GetObjectItemCaseSensitive(it, "score");
            ts = cJSON_GetObjectItemCaseSensitive(it, "ts");
            append_unique(arr, kind && cJSON_IsString(kind) ? kind->valuestring : "match",
                          text && cJSON_IsString(text) ? text->valuestring : NULL,
                          result && cJSON_IsString(result) ? result->valuestring : NULL,
                          score && cJSON_IsNumber(score) ? score->valuedouble : 0.0,
                          ts && cJSON_IsNumber(ts) ? (long long)ts->valuedouble : 0);
        }
    }

    if (root)
        cJSON_Delete(root);

    root = cJSON_Parse(retr);
    if (root && cJSON_IsArray(root)) {
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
    cJSON *text;
    cJSON *meta;
    cJSON *score;

            if (cap <= 0 || cJSON_GetArraySize(arr) >= cap)
                break;
            text = cJSON_GetObjectItemCaseSensitive(it, "text");
            meta = cJSON_GetObjectItemCaseSensitive(it, "meta");
            score = cJSON_GetObjectItemCaseSensitive(it, "score");
            append_unique(arr, "retrieved", text && cJSON_IsString(text) ? text->valuestring : NULL,
                          meta && cJSON_IsString(meta) ? meta->valuestring : NULL,
                          score && cJSON_IsNumber(score) ? score->valuedouble : 0.0, 0);
        }
    }

    if (root)
        cJSON_Delete(root);

    /* knowledge-graph associations distilled at LEARN (task→tool→file edges) */
    if (m) {
        char *rel = memory_graph_related(m, query ? query : "", 4);
        if (rel) {
            cJSON *rroot = cJSON_Parse(rel);
            if (rroot && cJSON_IsArray(rroot)) {
                cJSON *it;
                cJSON_ArrayForEach(it, rroot) {
    cJSON *t;

                    if (cap <= 0 || cJSON_GetArraySize(arr) >= cap)
                        break;
                    t = cJSON_GetObjectItemCaseSensitive(it, "text");
                    if (t && cJSON_IsString(t))
                        append_unique(arr, "graph", t->valuestring, NULL, 900.0, 0);
                }
            }
            if (rroot)
                cJSON_Delete(rroot);
            free(rel);
        }
    }

    free(search);
    free(retr);
    s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : xstrdup("[]");
}

char *context_render_text(const char *context_json) {
    strbuf sb;
    cJSON *root;

    strbuf_init(&sb);
    root = cJSON_Parse(context_json ? context_json : "[]");
    if (root && cJSON_IsArray(root)) {
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
    cJSON *kind;
    cJSON *text;
    cJSON *result;
    cJSON *tsj;

            if (sb.len > CTX_TOTAL_CAP) {
                strbuf_append(&sb, "…[context truncated]\n");
                break;
            }

            kind = cJSON_GetObjectItemCaseSensitive(it, "kind");
            text = cJSON_GetObjectItemCaseSensitive(it, "text");
            result = cJSON_GetObjectItemCaseSensitive(it, "result");
            tsj = cJSON_GetObjectItemCaseSensitive(it, "ts");
            const char *k = kind && cJSON_IsString(kind) ? kind->valuestring : "item";
            const char *t = text && cJSON_IsString(text) ? text->valuestring : "";
            const char *r = result && cJSON_IsString(result) ? result->valuestring : "";
            if (r && *r) {
                strbuf_appendf(&sb, "[%s] ", k);
                append_capped(&sb, t, CTX_ITEM_CAP);
                strbuf_append(&sb, " -> ");
                append_capped(&sb, r, CTX_ITEM_CAP);
                strbuf_append(&sb, "\n");
            } else {
                strbuf_appendf(&sb, "[%s] ", k);
                append_capped(&sb, t, CTX_ITEM_CAP);
                strbuf_append(&sb, "\n");
            }

            /* freshness: old memories are annotated, not presented as fact */
            if (tsj && cJSON_IsNumber(tsj) && tsj->valuedouble > 0) {
                double age_days = (time_now_ms() - tsj->valuedouble) / 86400000.0;
                if (age_days >= 1.0)
                    strbuf_appendf(&sb, "  (%d天前记录，可能过时)\n", (int)age_days);
            }
        }
    }

    if (root)
        cJSON_Delete(root);
    if (sb.len == 0)
        strbuf_append(&sb, "(no relevant memory)\n");
    return strbuf_detach(&sb);
}
