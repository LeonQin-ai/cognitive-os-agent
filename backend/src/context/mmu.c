/* mmu.c — Context MMU (Detailed Design v1.1 §9, §7.4/§7.5/§7.7).
 *
 * See mmu.h for the design. This file owns the page table, the
 * deterministic L0/L1/L2 derivators, the §7.7 factorized scorer, the
 * §9.4 recall/promotion flow and the §9.5 keep-score eviction. */
#include "cognitive-os-agent/context/mmu.h"
#include "cognitive-os-agent/memory/vector.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "cJSON.h"

/* --- page table --------------------------------------------------------- */

typedef enum { CTX_L0 = 0, CTX_L1, CTX_L2 } ctx_level_t;
typedef enum { CTX_COLD = 0, CTX_WARM, CTX_HOT } ctx_residency_t;

typedef struct ctx_page {
    char record_id[MEM_ID_MAX];

    ctx_residency_t residency;
    double relevance;         /* last retrieval relevance (fades for old) */

    double importance;
    double confidence;
    double utility;
    double salience;
    long long updated_ms;
    long long last_access_ms;
    unsigned access_count;
    int pinned;

    uint64_t content_hash;    /* regeneration guard (§7.4) */
    char *repr[3];            /* materialized L0/L1/L2 (owned; NULL = cold) */
    unsigned tok[3];          /* token cost per representation */
    mem_type_t type;
    char title[MEM_TITLE_MAX];
    char scope[MEM_SCOPE_MAX];
    mem_status_t status;
} ctx_page;

struct ctx_mmu {
    ctx_page *pages;
    size_t count, cap;
    size_t token_budget;
    size_t token_used;
    uint64_t generation;
    ctx_mmu_config cfg;
};

/* --- helpers ------------------------------------------------------------ */

/* UTF-8-safe truncation: cut at <= max chars, never mid-codepoint. */
static void utf8_truncate(const char *s, size_t max, strbuf *sb) {
    size_t n = strlen(s);
    if (n > max) {
        n = max;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
            n--;
    }
    strbuf_append_n(sb, s, n);
}

static unsigned token_estimate(const char *s) {
    if (!s)
        return 0;
    return (unsigned)((strlen(s) + 3) / 4);
}

static double clamp01(double x) {
    if (x < 0.0)
        return 0.0;
    if (x > 1.0)
        return 1.0;
    return x;
}

static const char *level_name(ctx_level_t l) {
    switch (l) {
    case CTX_L0: return "L0";
    case CTX_L1: return "L1";
    case CTX_L2: return "L2";
    }
    return "L0";
}

static const char *residency_name(ctx_residency_t r) {
    switch (r) {
    case CTX_COLD: return "COLD";
    case CTX_WARM: return "WARM";
    case CTX_HOT:  return "HOT";
    }
    return "COLD";
}

/* --- configuration ------------------------------------------------------ */

void ctx_mmu_config_default(ctx_mmu_config *cfg) {
    if (!cfg)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->token_budget = 4096;
    cfg->w_relevance = 0.35;
    cfg->w_importance = 0.15;
    cfg->w_confidence = 0.10;
    cfg->w_utility = 0.10;
    cfg->w_recency = 0.10;
    cfg->w_salience = 0.10;
    cfg->w_scope = 0.10;
    cfg->recency_halflife_ms = 24.0 * 3600.0 * 1000.0; /* 1 day */
    cfg->l2_top_k = 3;
    cfg->candidate_k = 16;
    cfg->scope = NULL;
    cfg->now_ms = 0;
}

ctx_mmu *ctx_mmu_new(void) {
    ctx_mmu *mmu = (ctx_mmu *)calloc(1, sizeof(ctx_mmu));
    if (!mmu)
        return NULL;
    ctx_mmu_config_default(&mmu->cfg);
    mmu->token_budget = mmu->cfg.token_budget;
    return mmu;
}

void ctx_mmu_free(ctx_mmu *mmu) {
    size_t i, j;
    if (!mmu)
        return;
    for (i = 0; i < mmu->count; i++)
        for (j = 0; j < 3; j++)
            free(mmu->pages[i].repr[j]);
    free(mmu->pages);
    free(mmu);
}

void ctx_mmu_set_config(ctx_mmu *mmu, const ctx_mmu_config *cfg) {
    if (!mmu || !cfg)
        return;
    mmu->cfg = *cfg;
    if (mmu->cfg.token_budget > 0)
        mmu->token_budget = mmu->cfg.token_budget;
}

int ctx_mmu_pin(ctx_mmu *mmu, const char *record_id, int pinned) {
    size_t i;
    if (!mmu || !record_id)
        return -1;
    for (i = 0; i < mmu->count; i++) {
        if (strcmp(mmu->pages[i].record_id, record_id) == 0) {
            mmu->pages[i].pinned = pinned ? 1 : 0;
            return 0;
        }
    }
    return -1;
}

char *ctx_mmu_stats_json(ctx_mmu *mmu) {
    strbuf sb;
    size_t i, hot = 0, warm = 0, cold = 0;
    if (!mmu)
        return NULL;
    for (i = 0; i < mmu->count; i++) {
        if (mmu->pages[i].residency == CTX_HOT)
            hot++;
        else if (mmu->pages[i].residency == CTX_WARM)
            warm++;
        else
            cold++;
    }
    strbuf_init(&sb);
    strbuf_appendf(&sb, "{\"pages\":%d,\"hot\":%d,\"warm\":%d,\"cold\":%d,"
                        "\"token_budget\":%d,\"token_used\":%d,\"generation\":%llu}",
                   (int)mmu->count, (int)hot, (int)warm, (int)cold,
                   (int)mmu->token_budget, (int)mmu->token_used,
                   (unsigned long long)mmu->generation);
    return strbuf_detach(&sb);
}

/* --- L0/L1/L2 derivators (§7.4) ----------------------------------------- */

/* L0 — abstract: 1-3 sentences / core conclusion, ~<=300 chars. */
static char *derive_l0(const ctx_page *pg, const mem_record *r) {
    strbuf sb;
    const unsigned char *c = (const unsigned char *)r->content;
    size_t i, n = strlen(r->content), cut = n < 300 ? n : 300;

    strbuf_init(&sb);
    if (pg->title[0]) {
        strbuf_append(&sb, pg->title);
        strbuf_append(&sb, ": ");
    }

    /* first sentence boundary (ASCII or CJK full-stop) within 300 bytes */
    for (i = 0; i + 2 < n && i < 300; i++) {
        if (c[i] == '.' || c[i] == '!' || c[i] == '?' || c[i] == '\n') {
            cut = i + 1;
            break;
        }
        /* CJK full-stop/！/？: U+3002 (E3 80 82), U+FF01 (EF BC 81),
         * U+FF1F (EF BC 9F) */
        if (c[i] == 0xE3 && c[i + 1] == 0x80 && c[i + 2] == 0x82) {
            cut = i + 3;
            break;
        }
        if (c[i] == 0xEF && c[i + 1] == 0xBC &&
            (c[i + 2] == 0x81 || c[i + 2] == 0x9F)) {
            cut = i + 3;
            break;
        }
    }
    utf8_truncate(r->content, cut, &sb);
    return strbuf_detach(&sb);
}

/* L1 — overview: attributes + evidence summary + truncated content. */
static char *derive_l1(const ctx_page *pg, const mem_record *r) {
    strbuf sb;
    strbuf_init(&sb);
    if (pg->title[0])
        strbuf_appendf(&sb, "%s\n", pg->title);
    strbuf_appendf(&sb, "[%s/%s/%s] importance=%.2f confidence=%.2f utility=%.2f\n",
                   mem_record_type_name(pg->type), pg->scope, mem_record_status_name(pg->status),
                   pg->importance, pg->confidence, pg->utility);
    if (r->evidence[0])
        strbuf_appendf(&sb, "evidence: %s\n", r->evidence);
    strbuf_append(&sb, "---\n");
    utf8_truncate(r->content, 1200, &sb);
    if (strlen(r->content) > 1200)
        strbuf_append(&sb, "...");

    return strbuf_detach(&sb);
}

/* L2 — detail: the complete Markdown record content. */
static char *derive_l2(const ctx_page *pg, const mem_record *r) {
    (void)pg;
    return xstrdup(r->content ? r->content : "");
}

/* Materialize `level` for the page if not cached; regenerate everything
 * when the record content hash changed (§7.4). Returns NULL on OOM. */
static char *materialize(ctx_mmu *mmu, ctx_page *pg, const mem_record *r,
                         ctx_level_t level, unsigned *tok_out) {
    uint64_t h = hash64(r->content, strlen(r->content));
    int j;

    if (pg->content_hash != h) {
        for (j = 0; j < 3; j++) {
            free(pg->repr[j]);
            pg->repr[j] = NULL;
            pg->tok[j] = 0;
        }
        pg->content_hash = h;
    }
    if (!pg->repr[level]) {
        switch (level) {
        case CTX_L0: pg->repr[level] = derive_l0(pg, r); break;
        case CTX_L1: pg->repr[level] = derive_l1(pg, r); break;
        case CTX_L2: pg->repr[level] = derive_l2(pg, r); break;
        }
        pg->tok[level] = token_estimate(pg->repr[level]);
    }
    (void)mmu;
    *tok_out = pg->tok[level];
    return pg->repr[level];
}

/* --- §7.7 factorized scoring -------------------------------------------- */

typedef struct ctx_factors {
    double relevance, importance, confidence, utility;
    double recency, salience, scope;
} ctx_factors;

static double scope_factor(const ctx_mmu_config *cfg, const char *scope) {
    if (!cfg->scope || !cfg->scope[0])
        return 1.0;
    if (scope && strncmp(scope, cfg->scope, strlen(cfg->scope)) == 0)
        return 1.0;
    if (scope && strcmp(scope, "global") == 0)
        return 0.75;
    return 0.4;
}

static double factor_score(const ctx_mmu_config *cfg, const ctx_factors *f) {
    double wsum = cfg->w_relevance + cfg->w_importance + cfg->w_confidence +
                  cfg->w_utility + cfg->w_recency + cfg->w_salience +
                  cfg->w_scope;
    double acc = cfg->w_relevance * f->relevance +
                 cfg->w_importance * f->importance +
                 cfg->w_confidence * f->confidence +
                 cfg->w_utility * f->utility +
                 cfg->w_recency * f->recency +
                 cfg->w_salience * f->salience +
                 cfg->w_scope * f->scope;
    return wsum > 0 ? clamp01(acc / wsum) : 0.0;
}

/* --- §9.5 eviction ------------------------------------------------------- */

/* Tokens a page currently charges against the budget (by residency). */
static unsigned page_tokens(const ctx_page *pg) {
    switch (pg->residency) {
    case CTX_HOT:  return pg->tok[CTX_L2];
    case CTX_WARM: return pg->tok[CTX_L1];
    default:       return 0;
    }
}

static double keep_score(const ctx_mmu *mmu, const ctx_page *pg) {
    double freq = pg->access_count > 10 ? 1.0 : pg->access_count / 10.0;
    double tok_penalty = mmu->token_budget > 0
                             ? (double)page_tokens(pg) / (double)mmu->token_budget
                             : 0.0;
    return pg->relevance + pg->importance + pg->utility + freq +
           (double)pg->salience * 0.5 - tok_penalty;
}

/* Evict unpinned pages (lowest keep score first) until token_used <= budget.
 * Eviction demotes the page to COLD and frees its materialized text. */
static void evict_to_budget(ctx_mmu *mmu) {
    for (;;) {
        ctx_page *worst = NULL;
        double worst_score = 0;
        size_t i, j;

        if (mmu->token_used <= mmu->token_budget)
            return;
        for (i = 0; i < mmu->count; i++) {
            ctx_page *pg = &mmu->pages[i];
            double ks;
            if (pg->pinned || pg->residency == CTX_COLD)
                continue;
            ks = keep_score(mmu, pg);
            if (!worst || ks < worst_score) {
                worst = pg;
                worst_score = ks;
            }
        }
        if (!worst)
            return;  /* everything pinned: over budget is unavoidable */

        mmu->token_used -= page_tokens(worst);
        for (j = 0; j < 3; j++) {
            free(worst->repr[j]);
            worst->repr[j] = NULL;
            worst->tok[j] = 0;
        }
        worst->residency = CTX_COLD;
        mmu->generation++;
    }
}

/* --- page sync ----------------------------------------------------------- */

static ctx_page *page_find(ctx_mmu *mmu, const char *id) {
    size_t i;
    for (i = 0; i < mmu->count; i++)
        if (strcmp(mmu->pages[i].record_id, id) == 0)
            return &mmu->pages[i];
    return NULL;
}

static ctx_page *page_get(ctx_mmu *mmu, const char *id) {
    ctx_page *pg = page_find(mmu, id);
    if (pg)
        return pg;
    if (mmu->count == mmu->cap) {
        size_t ncap = mmu->cap ? mmu->cap * 2 : 32;
        ctx_page *ni = (ctx_page *)realloc(mmu->pages, ncap * sizeof(ctx_page));
        if (!ni)
            return NULL;
        mmu->pages = ni;
        mmu->cap = ncap;
    }
    pg = &mmu->pages[mmu->count++];
    memset(pg, 0, sizeof *pg);
    snprintf(pg->record_id, sizeof pg->record_id, "%s", id);
    return pg;
}

/* Sync the page table with the store: refresh attributes, drop pages whose
 * record is gone or in an exit state, create COLD pages for new records.
 * Returns 0 ok. */
static int sync_pages(ctx_mmu *mmu, mem_records *rs) {
    size_t i, j, w = 0, k;
    int n = mem_records_count(rs);

    /* pass 1: create a COLD page for every live record missing one */
    for (k = 0; k < (size_t)n; k++) {
        const mem_record *r = mem_records_at(rs, (int)k);
        ctx_page *pg;

        if (!r || (r->status != MEM_STATUS_PROVISIONAL &&
                   r->status != MEM_STATUS_TRUSTED &&
                   r->status != MEM_STATUS_VERIFIED))
            continue;
        pg = page_get(mmu, r->id);
        if (!pg)
            continue;
        pg->type = r->type;
        snprintf(pg->title, sizeof pg->title, "%s", r->title);
        snprintf(pg->scope, sizeof pg->scope, "%s", r->scope);
        pg->status = r->status;
        pg->importance = clamp01(r->importance);
        pg->confidence = clamp01(r->confidence);
        pg->utility = clamp01(r->utility);
        pg->salience = r->salience > 0 ? clamp01(r->salience) : 0.5;
        pg->updated_ms = r->updated_ms;
        pg->last_access_ms = r->last_access_ms;
        pg->access_count = r->access_count;
    }

    /* pass 2: drop pages whose record is gone or in an exit state */
    for (i = 0; i < mmu->count; i++) {
        ctx_page *pg = &mmu->pages[i];
        const mem_record *r = mem_records_find(rs, pg->record_id);

        if (!r || (r->status != MEM_STATUS_PROVISIONAL &&
                   r->status != MEM_STATUS_TRUSTED &&
                   r->status != MEM_STATUS_VERIFIED)) {
            mmu->token_used -= page_tokens(pg);
            for (j = 0; j < 3; j++)
                free(pg->repr[j]);
            continue;  /* drop the page */
        }
        mmu->pages[w++] = *pg;
    }
    mmu->count = w;
    return 0;
}

/* --- recall (§9.4) -------------------------------------------------------- */

typedef struct recall_hit {
    const char *id;    /* borrowed from cJSON (valid during recall) */
    double relevance;  /* vector hybrid score */
} recall_hit;

/* Collect vector hits into recall_hit[]. Returns hit count, -1 error. */
static int parse_vector_hits(const char *json, recall_hit *hits, int max) {
    cJSON *arr = cJSON_Parse(json);
    int i, n, out = 0;

    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return -1;
    }
    n = cJSON_GetArraySize(arr);
    for (i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jsc = cJSON_GetObjectItemCaseSensitive(it, "score");
        const char *id = cJSON_GetStringValue(jid);
        if (!id || strncmp(id, "r:", 2) != 0)
            continue;
        if (out >= max)
            break;
        hits[out].id = id + 2;
        hits[out].relevance = jsc && cJSON_IsNumber(jsc) ? jsc->valuedouble : 0.0;
        out++;
    }
    cJSON_Delete(arr);
    return out;
}

char *ctx_mmu_recall(ctx_mmu *mmu, mem_records *rs, void *vs,
                     const char *query, const ctx_mmu_config *cfg) {
    ctx_mmu_config def;
    ctx_mmu_config c;
    recall_hit *hits = NULL;
    int nhits = 0;
    long long now;
    size_t i;
    strbuf out;
    char *vec_json;

    /* ranked promotion scratch: page pointers + scores */
    ctx_page **order = NULL;
    double *scores = NULL;
    size_t norder = 0;
    int promoted_l2 = 0;

    if (!mmu || !rs)
        return xstrdup("[]");
    if (!cfg) {
        ctx_mmu_config_default(&def);
        cfg = &def;
    }
    c = *cfg;
    if (c.token_budget > 0)
        mmu->token_budget = c.token_budget;
    if (c.candidate_k <= 0)
        c.candidate_k = 16;
    if (c.l2_top_k <= 0)
        c.l2_top_k = 3;
    now = c.now_ms > 0 ? c.now_ms : time_now_ms();

    sync_pages(mmu, rs);

    /* stage 0: vector candidates -> relevance on COLD pages */
    if (vs && query && query[0]) {
        hits = (recall_hit *)calloc((size_t)c.candidate_k, sizeof(recall_hit));
        vec_json = vectorstore_nearest_hybrid((vectorstore *)vs, query,
                                              c.candidate_k, 0.6f);
        if (hits && vec_json)
            nhits = parse_vector_hits(vec_json, hits, c.candidate_k);
        free(vec_json);
        for (i = 0; i < mmu->count; i++) {
            ctx_page *pg = &mmu->pages[i];
            int h;
            pg->relevance = 0.0;
            for (h = 0; h < nhits; h++) {
                if (strcmp(hits[h].id, pg->record_id) == 0) {
                    pg->relevance = clamp01(hits[h].relevance);
                    break;
                }
            }
        }
        free(hits);
    }

    /* stage 1: rank ALL pages by the §7.7 score */
    order = (ctx_page **)calloc(mmu->count ? mmu->count : 1, sizeof(ctx_page *));
    scores = (double *)calloc(mmu->count ? mmu->count : 1, sizeof(double));
    if (!order || !scores) {
        free(order);
        free(scores);
        return xstrdup("[]");
    }
    for (i = 0; i < mmu->count; i++) {
        ctx_page *pg = &mmu->pages[i];
        ctx_factors f;
        double age = (double)(now > pg->last_access_ms ? now - pg->last_access_ms : 0);
        f.relevance = pg->relevance;
        f.importance = pg->importance;
        f.confidence = pg->confidence;
        f.utility = pg->utility;
        f.recency = exp2(-age / (c.recency_halflife_ms > 0 ? c.recency_halflife_ms : 1.0));
        f.salience = pg->salience;
        f.scope = scope_factor(&c, pg->scope);
        scores[i] = factor_score(&c, &f);
        order[norder++] = pg;
    }
    /* insertion sort by score desc (candidate pools are small) */
    for (i = 1; i < norder; i++) {
        ctx_page *kp = order[i];
        double ks = scores[i];
        size_t j = i;
        while (j > 0 && scores[j - 1] < ks) {
            order[j] = order[j - 1];
            scores[j] = scores[j - 1];
            j--;
        }
        order[j] = kp;
        scores[j] = ks;
    }

    /* stage 2: promote top candidates to WARM/L1 while budget allows */
    for (i = 0; i < norder; i++) {
        ctx_page *pg = order[i];
        const mem_record *r;
        unsigned tok;

        if (pg->residency != CTX_COLD)
            continue;
        if (scores[i] < 0.05)
            break;  /* sorted: the rest are irrelevant */
        r = mem_records_find(rs, pg->record_id);
        if (!r)
            continue;
        materialize(mmu, pg, r, CTX_L1, &tok);
        if (mmu->token_used + tok > mmu->token_budget)
            continue;  /* try smaller candidates */
        mmu->token_used += tok;
        pg->residency = CTX_WARM;
        mmu->generation++;
    }
    evict_to_budget(mmu);

    /* stage 3: re-rank WARM, promote top l2_top_k to HOT/L2 (charge L2,
     * release the L1 charge — a page bills only its current level) */
    for (i = 0; i < norder && promoted_l2 < c.l2_top_k; i++) {
        ctx_page *pg = order[i];
        const mem_record *r;
        unsigned tok;

        if (pg->residency != CTX_WARM)
            continue;
        r = mem_records_find(rs, pg->record_id);
        if (!r)
            continue;
        materialize(mmu, pg, r, CTX_L2, &tok);
        if (mmu->token_used - pg->tok[CTX_L1] + tok > mmu->token_budget)
            continue;
        mmu->token_used += tok;
        mmu->token_used -= pg->tok[CTX_L1];
        pg->residency = CTX_HOT;
        pg->access_count++;
        pg->last_access_ms = now;
        mem_records_touch(rs, pg->record_id, now);
        promoted_l2++;
        mmu->generation++;
    }
    evict_to_budget(mmu);

    /* stage 4: assemble the context JSON with per-factor logging (§7.7) */
    strbuf_init(&out);
    strbuf_append(&out, "[");
    {
        int first = 1;
        for (i = 0; i < norder; i++) {
            ctx_page *pg = order[i];
            ctx_level_t lvl;
            const char *text;
            const mem_record *r;

            if (pg->residency == CTX_COLD)
                continue;
            lvl = pg->residency == CTX_HOT ? CTX_L2 : CTX_L1;
            text = pg->repr[lvl];
            if (!text)
                continue;
            r = mem_records_find(rs, pg->record_id);
            if (!r)
                continue;
            if (!first)
                strbuf_append(&out, ",");
            first = 0;
            strbuf_appendf(&out,
                           "{\"id\":\"%s\",\"title\":\"%s\",\"type\":\"%s\","
                           "\"scope\":\"%s\",\"status\":\"%s\",\"level\":\"%s\","
                           "\"residency\":\"%s\",\"score\":%.4f,"
                           "\"factors\":{\"relevance\":%.3f,\"importance\":%.3f,"
                           "\"confidence\":%.3f,\"utility\":%.3f,\"recency\":%.3f,"
                           "\"salience\":%.3f,\"scope\":%.3f},"
                           "\"tokens\":%d,\"text\":",
                           pg->record_id, pg->title, mem_record_type_name(pg->type),
                           pg->scope, mem_record_status_name(pg->status),
                           level_name(lvl), residency_name(pg->residency),
                           scores[i], pg->relevance, pg->importance,
                           pg->confidence, pg->utility,
                           exp2(-(double)(now > pg->last_access_ms
                                              ? now - pg->last_access_ms
                                              : 0) /
                                (c.recency_halflife_ms > 0 ? c.recency_halflife_ms : 1.0)),
                           pg->salience, scope_factor(&c, pg->scope),
                           (int)pg->tok[lvl]);
            {
                char *js = cJSON_PrintUnformatted(cJSON_CreateString(text));
                if (js) {
                    strbuf_append(&out, js);
                    cJSON_free(js);
                } else {
                    strbuf_append(&out, "\"\"");
                }
            }
            strbuf_append(&out, "}");
        }
    }
    strbuf_append(&out, "]");

    free(order);
    free(scores);
    return strbuf_detach(&out);
}
