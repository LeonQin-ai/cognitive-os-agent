/* promotion.c — V3.1 memory promotion gate (Detailed Design v1.1 §8).
 *
 * Implements the CANDIDATE -> score+verify -> promote/merge/reject pipeline
 * plus the §8.1 lifecycle transitions: promote enters as PROVISIONAL,
 * evidenced merges mature PROVISIONAL -> TRUSTED -> VERIFIED. The gate is
 * deliberately deterministic and cheap: no LLM call, pure attribute scoring
 * plus evidence verification. Consolidation (decay, archiving) stays in the
 * memory facade; this file only guards entry into the long-term store. */
#include "cognitive-os-agent/memory/promotion.h"
#include "cognitive-os-agent/memory/vector.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --- candidate ---------------------------------------------------------- */

void mem_candidate_init(mem_candidate *c) {
    if (!c)
        return;
    memset(c, 0, sizeof *c);
    c->type = MEMR_SEMANTIC;
    snprintf(c->source, sizeof c->source, "experience");
}

void mem_candidate_free(mem_candidate *c) {
    if (!c)
        return;
    free(c->content);
    c->content = NULL;
}

/* --- configuration ------------------------------------------------------ */

void mem_gate_config_default(mem_gate_config *cfg) {
    if (!cfg)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->promote_threshold = 0.55;
    cfg->merge_threshold = 0.35;
    cfg->require_evidence = 1;
    cfg->no_evidence_cap = 0.50;
    cfg->reinforcement = 0.05;
    cfg->utility_bump = 0.05;
    cfg->now_ms = 0;
}

/* --- scoring ------------------------------------------------------------ */

static double clamp01(double x) {
    if (x < 0.0)
        return 0.0;
    if (x > 1.0)
        return 1.0;
    return x;
}

static double outcome_weight(const mem_candidate *c) {
    switch (c->outcome) {
    case MEM_OUTCOME_SUCCESS:
        return 1.0;
    case MEM_OUTCOME_PARTIAL:
        return 0.7;
    case MEM_OUTCOME_FAILURE:
        /* Negative procedural memory: failure is strong evidence for
         * "what not to do" (Baseline §12). */
        return c->type == MEMR_PROCEDURAL ? 0.9 : 0.6;
    case MEM_OUTCOME_UNKNOWN:
        break;
    }
    return 0.4;
}

static int evidence_class_rank(mem_outcome_t o) {
    switch (o) {
    case MEM_OUTCOME_SUCCESS:  return 3;
    case MEM_OUTCOME_FAILURE:  return 3;
    case MEM_OUTCOME_PARTIAL:  return 2;
    case MEM_OUTCOME_UNKNOWN:  return 1;
    }
    return 1;
}

/* Records in an exit state are gone; a matching origin_id there does not
 * block a fresh promotion. */
static int status_live(mem_status_t s) {
    return s == MEM_STATUS_PROVISIONAL || s == MEM_STATUS_TRUSTED ||
           s == MEM_STATUS_VERIFIED;
}

/* Lifecycle maturation on merge (DDD §8.1): evidenced repeats promote
 * PROVISIONAL -> TRUSTED; a strong evidence class promotes TRUSTED ->
 * VERIFIED. */
static mem_status_t matured_status(mem_status_t cur, const mem_candidate *c) {
    if (!c->evidence[0])
        return cur;
    if (cur == MEM_STATUS_PROVISIONAL)
        return MEM_STATUS_TRUSTED;
    if (cur == MEM_STATUS_TRUSTED && evidence_class_rank(c->outcome) >= 3)
        return MEM_STATUS_VERIFIED;
    return cur;
}

double mem_gate_score(const mem_candidate *c, const mem_gate_config *cfg) {
    if (!c)
        return 0.0;
    mem_gate_config def;
    if (!cfg) {
        mem_gate_config_default(&def);
        cfg = &def;
    }

    double score = 0.40 * clamp01(c->importance) +
                   0.30 * clamp01(c->confidence) +
                   0.20 * outcome_weight(c) +
                   0.10 * clamp01(c->salience);

    /* Baseline §12: an unevidenced LLM conclusion must not be promoted on
     * its own say-so — cap it below the promote threshold by default. */
    if (cfg->require_evidence && c->evidence[0] == '\0' &&
        score > cfg->no_evidence_cap)
        score = cfg->no_evidence_cap;
    return clamp01(score);
}

/* --- gate --------------------------------------------------------------- */

/* Deterministic id: "mem_" + hex of (origin|scope|content) hash — the same
 * candidate always maps to the same id, and origin-collisions that reach
 * promote (no matching record) still disambiguate by content. */
static void candidate_id(const mem_candidate *c, char *out, size_t n) {
    strbuf sb;
    strbuf_init(&sb);
    strbuf_append(&sb, c->origin_id[0] ? c->origin_id : "-");
    strbuf_append(&sb, "|");
    strbuf_append(&sb, c->scope[0] ? c->scope : "global");
    strbuf_append(&sb, "|");
    strbuf_append(&sb, c->content ? c->content : "");

    char hex[17];
    hash_hex(hex, hash64(sb.buf ? sb.buf : "", sb.len));
    strbuf_free(&sb);

    snprintf(out, n, "mem_%.12s", hex);
}

static void refresh_mirror(void *vs, const char *id, const mem_record *r) {
    vectorstore *v = (vectorstore *)vs;
    char vid[64];
    snprintf(vid, sizeof vid, "r:%s", id);
    vectorstore_remove(v, vid);
    if (status_live(r->status))
        vectorstore_add(v, vid, r->content, "record");
}

mem_gate_decision_t mem_gate_apply(mem_records *rs, void *vs,
                                   const mem_gate_config *cfg,
                                   const mem_candidate *c,
                                   mem_gate_result *out) {
    mem_gate_result local;
    mem_gate_result *res = out ? out : &local;
    memset(res, 0, sizeof *res);
    res->decision = MEM_GATE_REJECT;

    if (!rs || !c || !c->content || !c->content[0])
        return res->decision;

    mem_gate_config def;
    if (!cfg) {
        mem_gate_config_default(&def);
        cfg = &def;
    }
    long long now = cfg->now_ms > 0 ? cfg->now_ms : time_now_ms();
    res->score = mem_gate_score(c, cfg);

    /* 1. reject: below the merge bar entirely */
    if (res->score < cfg->merge_threshold)
        return res->decision;

    /* 2. merge: same origin already recorded (and still live). Candidates
     * without an origin dedup by their deterministic candidate id. */
    char det_id[MEM_ID_MAX];
    det_id[0] = '\0';
    const mem_record *orig = NULL;
    if (c->origin_id[0]) {
        orig = mem_records_find_origin(rs, c->origin_id);
    } else {
        candidate_id(c, det_id, sizeof det_id);
        orig = mem_records_find(rs, det_id);
    }
    if (orig && status_live(orig->status)) {
        mem_record nr = *orig;            /* shallow copy; content below */
        char *content_copy = xstrdup(orig->content);
        if (!content_copy)
            return res->decision;

        nr.version = orig->version + 1;
        nr.confidence = clamp01(orig->confidence + cfg->reinforcement);
        nr.utility = clamp01(orig->utility + cfg->utility_bump);
        nr.status = matured_status(orig->status, c);
        nr.updated_ms = now;

        /* adopt the evidence class when the candidate is at least as strong
         * as what the record already carries (equal-strength latest wins) */
        if (c->evidence[0] &&
            evidence_class_rank(c->outcome) >= evidence_class_rank(orig->outcome)) {
            nr.outcome = c->outcome;
            snprintf(nr.evidence, sizeof nr.evidence, "%s", c->evidence);
        }

        nr.content = c->content;          /* borrowed for serialization */
        if (mem_records_put(rs, &nr) != 0) {
            free(content_copy);
            return res->decision;
        }
        /* put() deep-copied; release our copy and re-find for mirror */
        free(content_copy);
        const mem_record *stored = mem_records_find(rs, nr.id);
        if (stored)
            refresh_mirror(vs, stored->id, stored);

        snprintf(res->id, sizeof res->id, "%s", nr.id);
        res->version = nr.version;
        res->decision = MEM_GATE_MERGE;
        return res->decision;
    }

    /* 3. promote: strong enough to enter the long-term store */
    if (res->score >= cfg->promote_threshold) {
        mem_record r;
        memset(&r, 0, sizeof r);
        candidate_id(c, r.id, sizeof r.id);
        r.type = c->type;
        snprintf(r.scope, sizeof r.scope, "%s",
                 c->scope[0] ? c->scope : "global");
        r.status = MEM_STATUS_PROVISIONAL;  /* DDD §8.1: entry state */
        r.importance = clamp01(c->importance);
        r.confidence = clamp01(c->confidence);
        r.utility = 0.0;
        r.valence = c->valence;
        r.arousal = clamp01(c->arousal);
        r.salience = clamp01(c->salience);
        snprintf(r.source, sizeof r.source, "%s",
                 c->source[0] ? c->source : "experience");
        snprintf(r.origin_id, sizeof r.origin_id, "%s", c->origin_id);
        r.outcome = c->outcome;
        snprintf(r.evidence, sizeof r.evidence, "%s", c->evidence);
        r.version = 1;
        r.created_ms = now;
        r.updated_ms = now;
        r.last_access_ms = now;
        r.content = c->content;          /* borrowed for put() */

        if (mem_records_put(rs, &r) != 0)
            return res->decision;
        refresh_mirror(vs, r.id, &r);

        snprintf(res->id, sizeof res->id, "%s", r.id);
        res->version = r.version;
        res->decision = MEM_GATE_PROMOTE;
        return res->decision;
    }

    /* 4. between merge bar and promote bar with no origin match: reject */
    return res->decision;
}
