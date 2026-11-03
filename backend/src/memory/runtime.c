#include "memory/runtime.h"
#include "memory/record.h"
#include "memory/vector.h"
#include "memory/promotion.h"
#include "context/mmu.h"
#include "infra/util.h"
#include "infra/logging.h"
#include "os/os_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct memory_runtime {
    mem_records *records;
    vectorstore *vectors;
    mutex_t lock;
};

static char *bounded_copy(const char *text, size_t cap) {
    size_t n = strlen(text);
    if (n > cap) n = cap;
    while (n > 0 && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
    char *out = malloc(n + 1);
    if (out) { memcpy(out, text, n); out[n] = 0; }
    return out;
}

static void scoped(char out[MEM_SCOPE_MAX], const char *scope) {
    snprintf(out, MEM_SCOPE_MAX, "%s", scope && *scope ? scope : "global");
}

memory_runtime *memory_runtime_new(const char *state_root) {
    memory_runtime *rt = calloc(1, sizeof(*rt));
    char root[768];
    if (!rt) return NULL;
    if (mutex_init(&rt->lock) != 0) { free(rt); return NULL; }
    snprintf(root, sizeof(root), "%s/memory", state_root && *state_root ? state_root : "state");
    rt->records = mem_records_new(root);
    rt->vectors = vectorstore_new();
    if (!rt->records || !rt->vectors) { memory_runtime_free(rt); return NULL; }
    mem_records_rebuild_index(rt->records, rt->vectors);
    return rt;
}

void memory_runtime_free(memory_runtime *rt) {
    if (!rt) return;
    mem_records_free(rt->records);
    vectorstore_free(rt->vectors);
    mutex_destroy(&rt->lock);
    free(rt);
}

char *memory_runtime_recall(memory_runtime *rt, const char *scope,
                            const char *query, int char_budget) {
    ctx_mmu_config cfg;
    ctx_mmu *mmu;
    char *json;
    char scope_buf[MEM_SCOPE_MAX];
    if (!rt) return xstrdup("[]");
    scoped(scope_buf, scope);
    ctx_mmu_config_default(&cfg);
    cfg.scope = scope_buf;
    if (char_budget > 0) cfg.token_budget = (size_t)char_budget / 4u;
    if (cfg.token_budget < 64u) cfg.token_budget = 64u;
    /* Residency is scoped to one recall. This prevents a shared lane from
     * carrying HOT pages from session A into session B. Canonical records and
     * the vector mirror remain shared and durable. */
    mmu = ctx_mmu_new();
    if (!mmu) return xstrdup("[]");
    mutex_lock(&rt->lock);
    json = ctx_mmu_recall(mmu, rt->records, rt->vectors, query, &cfg);
    log_info("memory-mmu: scope=%s records=%d recalled_bytes=%zu budget_tokens=%zu",
             scope_buf, mem_records_count(rt->records), json ? strlen(json) : 0, cfg.token_budget);
    mutex_unlock(&rt->lock);
    ctx_mmu_free(mmu);
    return json ? json : xstrdup("[]");
}

int memory_runtime_learn(memory_runtime *rt, const char *scope,
                         const char *task, const char *answer,
                         int successful_tools, int all_actions_ok) {
    mem_candidate c;
    mem_gate_result result;
    mem_gate_config gate;
    strbuf body, identity;
    char hex[17];
    char *task_cap = NULL, *answer_cap = NULL;
    if (!rt || !task || !*task || !answer || !*answer || successful_tools <= 0)
        return 0;
    mem_candidate_init(&c);
    c.type = MEMR_EPISODIC;
    scoped(c.scope, scope);
    c.importance = 0.72;
    c.confidence = all_actions_ok ? 0.88 : 0.68;
    c.salience = 0.70;
    c.outcome = all_actions_ok ? MEM_OUTCOME_SUCCESS : MEM_OUTCOME_PARTIAL;
    snprintf(c.source, sizeof(c.source), "experience");
    snprintf(c.evidence, sizeof(c.evidence), "%d successful tool receipt%s",
             successful_tools, successful_tools == 1 ? "" : "s");
    strbuf_init(&identity);
    strbuf_append(&identity, c.scope); strbuf_append(&identity, "|"); strbuf_append(&identity, task);
    hash_hex(hex, hash64(identity.buf ? identity.buf : "", identity.len));
    snprintf(c.origin_id, sizeof(c.origin_id), "task_%.12s", hex);
    strbuf_free(&identity);
    strbuf_init(&body);
    task_cap = bounded_copy(task, 2048); answer_cap = bounded_copy(answer, 4096);
    if (!task_cap || !answer_cap) { free(task_cap); free(answer_cap); mem_candidate_free(&c); return 0; }
    strbuf_append(&body, "Task: "); strbuf_append(&body, task_cap);
    strbuf_append(&body, "\nResult: "); strbuf_append(&body, answer_cap);
    free(task_cap); free(answer_cap);
    c.content = strbuf_detach(&body);
    mem_gate_config_default(&gate);
    mutex_lock(&rt->lock);
    mem_gate_decision_t decision = mem_gate_apply(rt->records, rt->vectors, &gate, &c, &result);
    mutex_unlock(&rt->lock);
    mem_candidate_free(&c);
    return decision == MEM_GATE_REJECT ? 0 : 1;
}

int memory_runtime_count(memory_runtime *rt) {
    int n = 0;
    if (!rt) return 0;
    mutex_lock(&rt->lock); n = mem_records_count(rt->records); mutex_unlock(&rt->lock);
    return n;
}
