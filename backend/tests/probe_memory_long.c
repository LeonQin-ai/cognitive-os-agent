/* probe_memory_long.c — manual probe for long-run memory behavior.
 * Verifies (or disproves) three suspected defects:
 *   A. restart vector amnesia: episodes.json is reloaded on coa_memory_new but
 *      never mirrored into the vector store -> keyword search finds old
 *      episodes, but coa_memory_retrieve/_ex (used by the context builder for
 *      RAG) cannot -> the agent "forgets" after a restart.
 *   C. working-vector leak: coa_memory_working_push evicts the oldest ring
 *      item at cap 64 but leaves its "w:<seq>" entry in the vector store, so
 *      the store grows without bound in long sessions.
 *   D. lifecycle reinforce-protects: episodes reinforced >=2x survive a decay
 *      pass that drops 1x episodes (forget-with-archive works).
 *   E. history compaction keeps the newest 9 turns with a summary (no
 *      unsummarized loss in the normal path).
 *
 * Build (repo root, bash):
 *   zig cc -std=c11 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/cJSON \
 *     -Ithird_party/wasm3 -o build/probe-memory-long $(find src third_party/cJSON -name '*.c' | sort) \
 *     third_party/wasm3/wasm3_all.c tests/probe_memory_long.c -lws2_32 -lwinhttp -lm
 * Run: ./build/probe-memory-long   (exit 0 = all pass)
 */
#include "cognitive-os-agent/memory/memory.h"
#include "cognitive-os-agent/cognition/reasoning.h"
#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond)                                                              \
            printf("  PASS %s\n", msg);                                        \
        else {                                                                 \
            printf("  FAIL %s\n", msg);                                        \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

/* count JSON array elements whose "id" starts with "w:" */
static int count_w_ids(const char *json) {
    int n = 0;
    cJSON *arr = cJSON_Parse(json);
    if (!arr) return -1;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
        if (id && cJSON_IsString(id) && id->valuestring &&
            strncmp(id->valuestring, "w:", 2) == 0)
            n++;
    }
    cJSON_Delete(arr);
    return n;
}

/* first "text" (question) element of the history JSON array */
static char *first_history_q(const char *json) {
    cJSON *arr = cJSON_Parse(json);
    if (!arr) return NULL;
    cJSON *first = arr->child;
    char *out = NULL;
    if (first) {
        cJSON *q = cJSON_GetObjectItemCaseSensitive(first, "q");
        if (q && cJSON_IsString(q) && q->valuestring)
            out = strdup(q->valuestring);
    }
    cJSON_Delete(arr);
    return out;
}

int main(void) {
    /* ---------------- A. restart vector amnesia ---------------- */
    printf("=== A. restart: old episode recall ===\n");
    {
        coa_memory *m = coa_memory_new("state-probe-memA");
        CHECK(m != NULL, "memory created");
        if (!m) return 1;
        coa_memory_record_experience(m, "needleXYZ maritime law amendment research",
                                     "rare-result-token-qwe");
        for (int i = 0; i < 3; i++) {
            char t[64];
            snprintf(t, sizeof(t), "ordinary task %d cooking dinner", i);
            coa_memory_record_experience(m, t, "done");
        }
        coa_memory_flush(m);
        coa_memory_free(m);

        /* restart: reload from disk */
        coa_memory *m2 = coa_memory_new("state-probe-memA");
        CHECK(m2 != NULL, "memory reopened");
        if (m2) {
            /* control: keyword search scans episodes.json directly */
            char *s = coa_memory_search(m2, "maritime", 5);
            int s_hit = s && strstr(s, "needleXYZ") != NULL;
            CHECK(s_hit, "search finds old episode after restart (episodes persisted)");
            free(s);
            /* the actual RAG path used by the context builder */
            char *r1 = coa_memory_retrieve(m2, "maritime law amendment", 5);
            int r1_hit = r1 && strstr(r1, "needleXYZ") != NULL;
            CHECK(r1_hit, "retrieve finds old episode after restart (vector mirror)");
            free(r1);
            char *r2 = coa_memory_retrieve_ex(m2, "maritime law amendment", 5, 0.7f);
            int r2_hit = r2 && strstr(r2, "needleXYZ") != NULL;
            CHECK(r2_hit, "retrieve_ex finds old episode after restart");
            free(r2);
            /* new episodes recorded after restart must also be retrievable */
            coa_memory_record_experience(m2, "postRestart maritime follow-up needleABC",
                                         "result2");
            char *r3 = coa_memory_retrieve(m2, "maritime follow-up", 5);
            int r3_hit = r3 && strstr(r3, "needleABC") != NULL;
            CHECK(r3_hit, "retrieve finds post-restart episode");
            free(r3);
            coa_memory_free(m2);
        }
    }

    /* ---------------- C. working vector leak ---------------- */
    printf("=== C. working ring: vector store bounded ===\n");
    {
        coa_memory *m = coa_memory_new("state-probe-memC");
        if (m) {
            for (int i = 0; i < 300; i++) {
                char t[64];
                snprintf(t, sizeof(t), "work item %d status report", i);
                coa_memory_working_push(m, t);
            }
            CHECK(coa_memory_working_count(m) == 64, "working ring capped at 64");
            char *j = coa_memory_retrieve(m, "work item status", 1000);
            int w = j ? count_w_ids(j) : -1;
            printf("  (w: vector entries retrieved = %d)\n", w);
            CHECK(w >= 0 && w <= 64, "w: vector entries stay within ring cap");
            free(j);
            coa_memory_free(m);
        }
    }

    /* ---------------- D. lifecycle: reinforce protects ---------------- */
    printf("=== D. lifecycle: reinforced episode survives decay pass ===\n");
    {
        coa_memory *m = coa_memory_new("state-probe-memD");
        if (m) {
            coa_memory_record_experience(m, "needle-protected-task maritime", "v1");
            coa_memory_record_experience(m, "needle-protected-task maritime", "v2"); /* +1 */
            for (int i = 0; i < 10; i++) {
                char t[64];
                snprintf(t, sizeof(t), "once-only task %d paperwork", i);
                coa_memory_record_experience(m, t, "ok");
            }
            CHECK(coa_memory_episode_count(m) == 11, "11 episodes recorded");
            coa_memory_lifecycle_cfg cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.now_ms = coa_time_now_ms() + 3 * 1000; /* 3 half-lives */
            cfg.half_life_ms = 1000;
            cfg.min_strength = 0.2;
            cfg.archive = 1;
            int dropped = coa_memory_lifecycle_pass(m, &cfg);
            printf("  (dropped=%d)\n", dropped);
            CHECK(dropped == 10, "10 once-episodes dropped (1x -> 0.125 < 0.2)");
            CHECK(coa_memory_episode_count(m) == 1, "reinforced episode survives");
            char *ej = coa_memory_episodes_json(m);
            CHECK(ej && strstr(ej, "needle-protected-task") != NULL,
                  "survivor is the reinforced episode");
            free(ej);
            char *arc = coa_fs_read_file("state-probe-memD/memory/archive.jsonl");
            CHECK(arc && strstr(arc, "once-only task 3") != NULL,
                  "archive.jsonl written with dropped entries");
            free(arc);
            coa_memory_free(m);
        }
    }

    /* ---------------- E. history compaction keeps newest ---------------- */
    printf("=== E. reasoning history: compaction keeps newest 9 ===\n");
    {
        coa_llm *llm = coa_llm_create("mock", NULL, NULL, "mock");
        coa_tool_registry *reg = coa_tool_registry_new();
        if (reg) coa_tool_register_builtins(reg);
        if (llm && reg) {
            coa_reasoning_config cfg = {0};
            cfg.llm = llm;
            cfg.tools = reg;
            coa_reasoning *r = coa_reasoning_new(&cfg);
            if (r) {
                for (int i = 0; i < 17; i++) {
                    char p[128];
                    snprintf(p, sizeof(p), "\xe9\x97\xae\xe9\xa2\x98%d\xef\xbc\x9a"
                                           "\xe8\x81\x8a\xe8\x81\x8a\xe8\xaf\x9d\xe9\xa2\x98%d",
                             i, i); /* 问题%d：聊聊话题%d */
                    char *ans = NULL;
                    if (coa_reasoning_run(r, p, &ans) != 0) {
                        printf("  run %d failed\n", i);
                        g_fail++;
                    }
                    free(ans);
                }
                char *sj = coa_reasoning_session_json(r);
                cJSON *o = sj ? cJSON_Parse(sj) : NULL;
                cJSON *ht = o ? cJSON_GetObjectItemCaseSensitive(o, "history_turns") : NULL;
                CHECK(ht && cJSON_IsNumber(ht) && ht->valuedouble == 9,
                      "history_turns == 9 after 17 runs");
                cJSON *sum = o ? cJSON_GetObjectItemCaseSensitive(o, "summary") : NULL;
                CHECK(sum && cJSON_IsString(sum) && sum->valuestring[0] != '\0',
                      "summary written");
                cJSON_Delete(o);
                free(sj);
                char *hj = coa_reasoning_history_json(r, 100);
                char *fq = hj ? first_history_q(hj) : NULL;
                if (fq)
                    printf("  (oldest surviving turn: %s)\n", fq);
                CHECK(fq && strstr(fq, "\xe9\x97\xae\xe9\xa2\x98" "8") == fq,
                      "oldest turn is 问题8 (turns 0-7 summarized, none lost)");
                free(fq);
                free(hj);
                coa_reasoning_free(r);
            }
        }
        if (llm) coa_llm_destroy(llm);
        if (reg) coa_tool_registry_free(reg);
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT", g_fail);
    return g_fail == 0 ? 0 : 1;
}
