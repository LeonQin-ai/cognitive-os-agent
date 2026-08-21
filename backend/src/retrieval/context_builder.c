/* context_builder.c — assemble retrieval context from memory. */
#include "cagent/retrieval/context_builder.h"
#include "cagent/memory/memory.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

/* Append {kind,text,result,score} if `text` is not already present. */
static int append_unique(cJSON *arr, const char *kind, const char *text,
                         const char *result, double score) {
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
    cJSON_AddItemToArray(arr, o);
    return 1;
}

char *ca_context_build(ca_memory *m, const char *query, int max_items) {
    if (max_items <= 0) max_items = 8;
    char *search = m ? ca_memory_search(m, query ? query : "", max_items) : ca_strdup("[]");
    char *retr   = m ? ca_memory_retrieve(m, query ? query : "", max_items) : ca_strdup("[]");

    cJSON *arr = cJSON_CreateArray();
    if (!arr) { free(search); free(retr); return ca_strdup("[]"); }

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
            append_unique(arr,
                          kind && cJSON_IsString(kind) ? kind->valuestring : "match",
                          text && cJSON_IsString(text) ? text->valuestring : NULL,
                          result && cJSON_IsString(result) ? result->valuestring : NULL,
                          score && cJSON_IsNumber(score) ? score->valuedouble : 0.0);
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
                          score && cJSON_IsNumber(score) ? score->valuedouble : 0.0);
        }
    }
    if (root) cJSON_Delete(root);

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
            cJSON *kind = cJSON_GetObjectItemCaseSensitive(it, "kind");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(it, "text");
            cJSON *result = cJSON_GetObjectItemCaseSensitive(it, "result");
            const char *k = kind && cJSON_IsString(kind) ? kind->valuestring : "item";
            const char *t = text && cJSON_IsString(text) ? text->valuestring : "";
            const char *r = result && cJSON_IsString(result) ? result->valuestring : "";
            if (r && *r)
                ca_strbuf_appendf(&sb, "[%s] %s -> %s\n", k, t, r);
            else
                ca_strbuf_appendf(&sb, "[%s] %s\n", k, t);
        }
    }
    if (root) cJSON_Delete(root);
    if (sb.len == 0) ca_strbuf_append(&sb, "(no relevant memory)\n");
    return ca_strbuf_detach(&sb);
}
