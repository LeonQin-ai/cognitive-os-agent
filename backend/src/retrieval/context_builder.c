/* context_builder.c — assemble retrieval context from memory.
 * Everything that enters the LLM prompt passes through here, so sizes are
 * capped per item and for the whole section, and stored episodes carry an
 * age annotation (stale memories are flagged, not asserted as fact). */
#include "cagent/retrieval/context_builder.h"
#include "cagent/memory/memory.h"
#include "cagent/infra/util.h"
#include "cagent/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define CTX_ITEM_CAP 300   /* max chars per item text/result */
#define CTX_TOTAL_CAP 4096 /* max chars of the rendered context section */
#define CTX_FACTS_MAX 20   /* max long-term facts injected per query */

/* Append s to sb, truncating at a UTF-8 boundary and marking the cut. */
static void append_capped(ca_strbuf *sb, const char *s, size_t cap) {
    if (!s) return;
    size_t n = strlen(s);
    int trunc = n > cap;
    if (trunc) n = cap;
    while (trunc && n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    ca_strbuf_append_n(sb, s, n);
    if (trunc) ca_strbuf_append(sb, "…");
}

/* Append {kind,text,result,score,ts} if `text` is not already present. */
static int append_unique(cJSON *arr, const char *kind, const char *text,
                         const char *result, double score, long long ts) {
    if (!arr || !text) return 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "text");
        if (t && cJSON_IsString(t) && strcmp(t->valuestring, text) == 0) return 0;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "kind", kind);
    cJSON_AddStringToObject(o, "text", text);
    cJSON_AddStringToObject(o, "result", result ? result : "");
    cJSON_AddNumberToObject(o, "score", score);
    if (ts > 0) cJSON_AddNumberToObject(o, "ts", (double)ts);
    cJSON_AddItemToArray(arr, o);
    return 1;
}

char *ca_context_build(ca_memory *m, const char *query, int max_items) {
    if (max_items <= 0) max_items = 8;
    char *search = m ? ca_memory_search(m, query ? query : "", max_items) : ca_strdup("[]");
    /* two-stage retrieval: hybrid recall -> rerank -> blended top-k */
    char *retr   = m ? ca_memory_retrieve_ex(m, query ? query : "", max_items, 0.7f) : ca_strdup("[]");

    cJSON *arr = cJSON_CreateArray();
    if (!arr) { free(search); free(retr); return ca_strdup("[]"); }

    /* long-term facts first (always relevant, small, explicitly stored) */
    if (m) {
        char *facts = ca_memory_longterm_json(m);
        cJSON *root = cJSON_Parse(facts);
        if (root && cJSON_IsObject(root)) {
            int added = 0;
            cJSON *it;
            cJSON_ArrayForEach(it, root) {
                if (added >= CTX_FACTS_MAX) break;
                if (it->string) {
                    char kv[600];
                    snprintf(kv, sizeof(kv), "%s: %s", it->string,
                             (it->valuestring && cJSON_IsString(it)) ? it->valuestring : "");
                    if (append_unique(arr, "fact", kv, NULL, 1000.0, 0))
                        added++;
                }
            }
        }
        if (root) cJSON_Delete(root);
        free(facts);
    }

    int cap = max_items;
    cJSON *root = cJSON_Parse(search);
    if (root && cJSON_IsArray(root)) {
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
            if (cap <= 0 || cJSON_GetArraySize(arr) >= cap) break;
            cJSON *kind = cJSON_GetObjectItemCaseSensitive(it, "kind");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(it, "text");
            cJSON *result = cJSON_GetObjectItemCaseSensitive(it, "result");
            cJSON *score = cJSON_GetObjectItemCaseSensitive(it, "score");
            cJSON *ts = cJSON_GetObjectItemCaseSensitive(it, "ts");
            append_unique(arr,
                          kind && cJSON_IsString(kind) ? kind->valuestring : "match",
                          text && cJSON_IsString(text) ? text->valuestring : NULL,
                          result && cJSON_IsString(result) ? result->valuestring : NULL,
                          score && cJSON_IsNumber(score) ? score->valuedouble : 0.0,
                          ts && cJSON_IsNumber(ts) ? (long long)ts->valuedouble : 0);
        }
    }
    if (root) cJSON_Delete(root);

    root = cJSON_Parse(retr);
    if (root && cJSON_IsArray(root)) {
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
            if (cap <= 0 || cJSON_GetArraySize(arr) >= cap) break;
            cJSON *text = cJSON_GetObjectItemCaseSensitive(it, "text");
            cJSON *meta = cJSON_GetObjectItemCaseSensitive(it, "meta");
            cJSON *score = cJSON_GetObjectItemCaseSensitive(it, "score");
            append_unique(arr, "retrieved",
                          text && cJSON_IsString(text) ? text->valuestring : NULL,
                          meta && cJSON_IsString(meta) ? meta->valuestring : NULL,
                          score && cJSON_IsNumber(score) ? score->valuedouble : 0.0,
                          0);
        }
    }
    if (root) cJSON_Delete(root);

    /* knowledge-graph associations distilled at LEARN (task→tool→file edges) */
    if (m) {
        char *rel = ca_memory_graph_related(m, query ? query : "", 4);
        if (rel) {
            cJSON *rroot = cJSON_Parse(rel);
            if (rroot && cJSON_IsArray(rroot)) {
                cJSON *it;
                cJSON_ArrayForEach(it, rroot) {
                    if (cap <= 0 || cJSON_GetArraySize(arr) >= cap) break;
                    cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "text");
                    if (t && cJSON_IsString(t))
                        append_unique(arr, "graph", t->valuestring, NULL, 900.0, 0);
                }
            }
            if (rroot) cJSON_Delete(rroot);
            free(rel);
        }
    }

    free(search);
    free(retr);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : ca_strdup("[]");
}

char *ca_context_render_text(const char *context_json) {
    ca_strbuf sb;
    ca_strbuf_init(&sb);
    cJSON *root = cJSON_Parse(context_json ? context_json : "[]");
    if (root && cJSON_IsArray(root)) {
        cJSON *it;
        cJSON_ArrayForEach(it, root) {
            if (sb.len > CTX_TOTAL_CAP) {
                ca_strbuf_append(&sb, "…[context truncated]\n");
                break;
            }
            cJSON *kind = cJSON_GetObjectItemCaseSensitive(it, "kind");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(it, "text");
            cJSON *result = cJSON_GetObjectItemCaseSensitive(it, "result");
            cJSON *tsj = cJSON_GetObjectItemCaseSensitive(it, "ts");
            const char *k = kind && cJSON_IsString(kind) ? kind->valuestring : "item";
            const char *t = text && cJSON_IsString(text) ? text->valuestring : "";
            const char *r = result && cJSON_IsString(result) ? result->valuestring : "";
            if (r && *r) {
                ca_strbuf_appendf(&sb, "[%s] ", k);
                append_capped(&sb, t, CTX_ITEM_CAP);
                ca_strbuf_append(&sb, " -> ");
                append_capped(&sb, r, CTX_ITEM_CAP);
                ca_strbuf_append(&sb, "\n");
            } else {
                ca_strbuf_appendf(&sb, "[%s] ", k);
                append_capped(&sb, t, CTX_ITEM_CAP);
                ca_strbuf_append(&sb, "\n");
            }
            /* freshness: old memories are annotated, not presented as fact */
            if (tsj && cJSON_IsNumber(tsj) && tsj->valuedouble > 0) {
                double age_days = (ca_time_now_ms() - tsj->valuedouble) / 86400000.0;
                if (age_days >= 1.0)
                    ca_strbuf_appendf(&sb, "  (%d天前记录，可能过时)\n", (int)age_days);
            }
        }
    }
    if (root) cJSON_Delete(root);
    if (sb.len == 0) ca_strbuf_append(&sb, "(no relevant memory)\n");
    return ca_strbuf_detach(&sb);
}
