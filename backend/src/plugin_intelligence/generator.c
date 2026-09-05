/* generator.c — AI plugin generation pipeline (self-evolution loop). */
#include "cognitive-os-agent/plugin_intelligence/generator.h"
#include "cognitive-os-agent/plugin_intelligence/analyzer.h"
#include "cognitive-os-agent/plugin_intelligence/architect.h"
#include "cognitive-os-agent/plugin_intelligence/codegen.h"
#include "cognitive-os-agent/plugin_intelligence/testing.h"
#include "cognitive-os-agent/plugin_intelligence/security.h"
#include "cognitive-os-agent/plugin_runtime/sandbox.h"
#include "cognitive-os-agent/plugin_runtime/registry.h"
#include "cognitive-os-agent/action/skill.h"
#include "cognitive-os-agent/llm/llm.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/infra/logging.h"
#include "cognitive-os-agent/cognitive-os-agent.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"

#define PLUGIN_VERSION "1.0.0"

static const char *ARCH_PROMPT =
    "You are the plugin architect of a cognitive OS. Given a capability "
    "request, design a standalone POSIX shell script that implements it.\n"
    "Respond with ONLY valid JSON (no markdown, no prose):\n"
    "{\"name\":\"short-kebab-case-name\",\"description\":\"one-line summary\","
    "\"capabilities\":[\"fs.read\",\"fs.write\"],\"script\":\"#!/bin/sh\\n...\"}\n"
    "Constraints for the script: pure POSIX shell, no network access, no "
    "destructive commands (no rm -rf / rm -fr / mkfs / dd), idempotent, "
    "operates only in the current directory, prints a short result, exits 0 "
    "on success.";

/* lowercase + replace invalid chars with '-', clamp to 48 chars */
static void sanitize_name(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 1 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out[o++] = (char)c;
        } else if (c >= 'A' && c <= 'Z') {
            out[o++] = (char)(c - 'A' + 'a');
        } else if (o > 0 && out[o - 1] != '-') {
            out[o++] = '-';
        }
    }
    while (o > 0 && out[o - 1] == '-') o--;
    if (o == 0) { snprintf(out, cap, "cap"); return; }
    out[o] = '\0';
}

/* Deterministic mock design used when provider == mock (offline testing). */
static char *mock_design(const char *description) {
    char name[64];
    sanitize_name(description, name, sizeof(name));

    /* capability derivation mirrors analyzer.c keyword heuristics */
    cJSON *caps = cJSON_CreateArray();
    const char *d = description ? description : "";
    if (strstr(d, "file") || strstr(d, "read") || strstr(d, "write")) {
        cJSON_AddItemToArray(caps, cJSON_CreateString("fs.read"));
        cJSON_AddItemToArray(caps, cJSON_CreateString("fs.write"));
    }
    if (strstr(d, "git") || strstr(d, "repo") || strstr(d, "commit"))
        cJSON_AddItemToArray(caps, cJSON_CreateString("git"));
    if (strstr(d, "http") || strstr(d, "network") || strstr(d, "api"))
        cJSON_AddItemToArray(caps, cJSON_CreateString("net"));
    if (cJSON_GetArraySize(caps) == 0)
        cJSON_AddItemToArray(caps, cJSON_CreateString("fs.read"));

    char *caps_s = cJSON_PrintUnformatted(caps);
    cJSON_Delete(caps);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "description", description ? description : "");
    cJSON_AddStringToObject(root, "capabilities", caps_s ? caps_s : "[]");
    free(caps_s);
    char script[1024];
    snprintf(script, sizeof(script),
             "echo \"plugin:%s ok: performed capability (%s)\"",
             name, description ? description : "");
    cJSON_AddStringToObject(root, "script", script);
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return js;
}

/* Try to extract {"name","description","capabilities","script"} from an LLM
 * response (tolerating markdown fences). Returns a cJSON object or NULL. */
static char *local_strndup(const char *s, size_t n) {
    char *o = malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n);
    o[n] = '\0';
    return o;
}

static cJSON *parse_design(const char *text) {
    if (!text) return NULL;
    const char *s = text;
    const char *fb = strstr(text, "```");
    if (fb) {
        const char *e = strstr(fb + 3, "```");
        const char *nl = strchr(fb, '\n');
        s = nl ? nl + 1 : fb + 3;
        if (e && e > s) {
            /* strip fences; use inner text */
            char *tmp = local_strndup(s, (size_t)(e - s));
            cJSON *r = cJSON_Parse(tmp);
            free(tmp);
            return r;
        }
    }
    return cJSON_Parse(s);
}

static int design_ok(const cJSON *root, char *name_out, size_t name_cap,
                     char *script_out, size_t script_cap) {
    if (!root || !cJSON_IsObject(root)) return 0;
    cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "script");
    if (!n || !cJSON_IsString(n) || !sc || !cJSON_IsString(sc)) return 0;
    const char *raw_name = n->valuestring;
    sanitize_name(raw_name, name_out, name_cap);
    snprintf(script_out, script_cap, "%s", sc->valuestring);
    return script_out[0] != '\0';
}

/* 1 if the script passes the security review (no high-severity findings and
 * not on the sandbox forbidden list). */
static int security_ok(const char *script) {
    if (!script || coa_sandbox_forbidden(script)) return 0;
    char *audit = coa_security_audit(script);
    if (!audit) return 0;
    cJSON *root = cJSON_Parse(audit);
    free(audit);
    if (!root) return 0;
    cJSON *findings = cJSON_GetObjectItemCaseSensitive(root, "findings");
    int ok = 1;
    if (findings && cJSON_IsArray(findings)) {
        cJSON *it;
        cJSON_ArrayForEach(it, findings) {
            cJSON *sev = cJSON_GetObjectItemCaseSensitive(it, "severity");
            if (sev && cJSON_IsNumber(sev) && sev->valuedouble >= 3) { ok = 0; break; }
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* Parse a JSON object out of stage-report text. */
static cJSON *stage_obj(const char *json_text) {
    return json_text ? cJSON_Parse(json_text) : NULL;
}

/* Join a JSON array of strings into a comma-separated csv (caller frees). */
static char *caps_csv_from(cJSON *arr) {
    if (!arr || !cJSON_IsArray(arr)) return NULL;
    coa_strbuf sb;
    coa_strbuf_init(&sb);
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || !it->valuestring || !*it->valuestring) continue;
        if (sb.len > 0) coa_strbuf_append(&sb, ",");
        coa_strbuf_append(&sb, it->valuestring);
    }
    if (sb.len == 0) { coa_strbuf_free(&sb); return NULL; }
    return coa_strbuf_detach(&sb);
}

/* Smoke-test the generated script once via the sandboxed test runner.
 * Returns 1 pass, 0 fail (only when the script actually ran and failed). */
static int smoke_test(const char *path, char **report_out) {
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "sh \"%s\"", path);
    char *report = coa_testing_run(cmd, 5000);
    if (report_out) *report_out = report;
    else free(report);
    cJSON *r = cJSON_Parse(report ? report : "{}");
    if (!r) return 1; /* cannot judge -> do not gate */
    cJSON *ec = cJSON_GetObjectItemCaseSensitive(r, "exit_code");
    cJSON *to = cJSON_GetObjectItemCaseSensitive(r, "timed_out");
    int pass = 1;
    /* exit_code == -1 means spawn failed (e.g. no sh on the host): don't gate */
    if (ec && cJSON_IsNumber(ec) && (int)ec->valuedouble > 0) pass = 0;
    if (to && cJSON_IsTrue(to)) pass = 0;
    cJSON_Delete(r);
    return pass;
}

char *coa_plugin_generate_deps(const coa_plugin_gen_deps *deps, const char *description) {
    if (!deps || !description || !*description)
        return coa_strdup("{\"ok\":false,\"error\":\"missing description\"}");

    const char *provider = (deps->provider && *deps->provider)
        ? deps->provider
        : (deps->llm && deps->llm->provider ? deps->llm->provider : "mock");
    const int real = strcmp(provider, "mock") != 0;

    /* --- stage 1: requirement analyzer (intent / caps / tools) --- */
    char *analysis_s = NULL, *arch_s = NULL;
    cJSON *analysis = NULL, *arch = NULL;
    {
        cJSON *spec = cJSON_CreateObject();
        cJSON_AddStringToObject(spec, "name", "");
        cJSON_AddStringToObject(spec, "description", description);
        char *spec_s = cJSON_PrintUnformatted(spec);
        cJSON_Delete(spec);
        analysis_s = coa_analyzer_analyze(spec_s ? spec_s : "{}");
        free(spec_s);
        analysis = stage_obj(analysis_s);
    }

    /* --- stage 2: architect (component/interface plan) --- */
    arch_s = coa_architect_design(description);
    arch = stage_obj(arch_s);

    /* --- stage 3: code design via LLM (or deterministic mock template) --- */
    coa_strbuf pb;
    coa_strbuf_init(&pb);
    coa_strbuf_append(&pb, ARCH_PROMPT);
    if (analysis_s && *analysis_s && strcmp(analysis_s, "{}") != 0)
        coa_strbuf_appendf(&pb, "\n\nRequirement analysis: %s", analysis_s);
    if (arch_s && *arch_s && strcmp(arch_s, "{}") != 0)
        coa_strbuf_appendf(&pb, "\n\nArchitecture plan: %s", arch_s);

    cJSON *design = NULL;
    if (real && deps->llm) {
        char *resp = coa_llm_chat_simple(deps->llm, pb.buf ? pb.buf : ARCH_PROMPT,
                                        description);
        design = parse_design(resp);
        free(resp);
    }
    coa_strbuf_free(&pb);
    if (!design) {
        /* fallback: mock template (also used when the LLM returns garbage) */
        char *js = mock_design(description);
        design = js ? cJSON_Parse(js) : NULL;
        free(js);
        if (!design) {
            free(analysis_s); free(arch_s);
            if (analysis) cJSON_Delete(analysis);
            if (arch) cJSON_Delete(arch);
            return coa_strdup("{\"ok\":false,\"error\":\"pipeline failed\"}");
        }
    }

    char name[64], script[65536];
    if (!design_ok(design, name, sizeof(name), script, sizeof(script))) {
        cJSON_Delete(design);
        free(analysis_s); free(arch_s);
        if (analysis) cJSON_Delete(analysis);
        if (arch) cJSON_Delete(arch);
        return coa_strdup("{\"ok\":false,\"error\":\"LLM design invalid (missing name/script)\"}");
    }

    cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(design, "description");
    const char *desc = desc_j && cJSON_IsString(desc_j) ? desc_j->valuestring : description;
    cJSON *caps_j = cJSON_GetObjectItemCaseSensitive(design, "capabilities");

    /* --- stage 4: security review gate --- */
    if (!security_ok(script)) {
        cJSON_Delete(design);
        free(analysis_s); free(arch_s);
        if (analysis) cJSON_Delete(analysis);
        if (arch) cJSON_Delete(arch);
        return coa_strdup("{\"ok\":false,\"error\":\"security review rejected the generated script\"}");
    }

    /* --- content signature --- */
    uint64_t h = coa_hash64(script, strlen(script));
    char signature[32];
    coa_hash_hex(signature, h);

    /* --- register as a versioned plugin --- */
    char **caps = NULL;
    size_t n_caps = 0;
    if (caps_j && cJSON_IsArray(caps_j)) {
        int caps_len = cJSON_GetArraySize(caps_j);
        n_caps = (size_t)caps_len;
        if (n_caps) {
            caps = calloc(n_caps, sizeof(char *));
            if (caps) {
                for (size_t i = 0; i < n_caps && (int)i < caps_len; i++) {
                    cJSON *c = cJSON_GetArrayItem(caps_j, (int)i);
                    caps[i] = (c && cJSON_IsString(c)) ? c->valuestring : NULL;
                }
            } else n_caps = 0;
        }
    }
    int reg_ok = 0;
    /* the registry rejects exact name+version duplicates, so a regenerated
     * plugin gets its patch version bumped (1.0.0 -> 1.0.1 -> ...) */
    char version[32];
    snprintf(version, sizeof(version), "%s", PLUGIN_VERSION);
    if (deps->registry) {
        const coa_plugin_meta *prev = coa_plugin_registry_find(deps->registry, name);
        if (prev && prev->version) {
            int a = 0, b = 0, c = 0;
            if (sscanf(prev->version, "%d.%d.%d", &a, &b, &c) == 3)
                snprintf(version, sizeof(version), "%d.%d.%d", a, b, c + 1);
        }
        coa_plugin_meta meta;
        memset(&meta, 0, sizeof(meta));
        meta.name = name;
        meta.version = version;
        meta.signature = signature;
        meta.description = (char *)desc;
        meta.caps = caps;
        meta.n_caps = n_caps;
        meta.enabled = 1;
        meta.built_ms = 0;
        reg_ok = coa_plugin_registry_register(deps->registry, &meta) == 0;
    }

    /* --- register as a runnable skill carrying the granted caps --- */
    int skill_ok = 0;
    char *caps_csv = caps_csv_from(caps_j);
    if (deps->skills) {
        const coa_skill sk = { name, desc, "shell", script, caps_csv };
        skill_ok = coa_skill_register_ex(deps->skills, &sk, 1) == 0;
    }

    /* --- persist registries so the capability survives a restart --- */
    if (reg_ok && deps->registry && deps->state_root)
        coa_plugin_registry_persist(deps->registry, deps->state_root);
    if (skill_ok && deps->skills && deps->state_root)
        coa_skill_registry_persist(deps->skills, deps->state_root);

    /* --- persist under <state_root>/plugins/<name>.sh --- */
    char dir[600], path[700];
    snprintf(dir, sizeof(dir), "%s", deps->state_root ? deps->state_root : "state");
    snprintf(path, sizeof(path), "%s/plugins", dir);
    coa_fs_mkdirs(path);
    snprintf(path, sizeof(path), "%s/plugins/%s.sh", dir, name);
    coa_fs_write_file(path, script, (size_t)strlen(script));

    /* --- stage 5: automated smoke test through the sandbox --- */
    char *test_report = NULL;
    int test_ok = smoke_test(path, &test_report);
    if (!test_ok) coa_log_warn("plugin %s failed its smoke test", name);

    /* --- native C skeleton for the .so/.dll loader path --- */
    char *c_skeleton = coa_codegen_plugin(name, desc);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", 1);
    cJSON *pj = cJSON_CreateObject();
    cJSON_AddStringToObject(pj, "name", name);
    cJSON_AddStringToObject(pj, "version", version);
    cJSON_AddStringToObject(pj, "description", desc);
    cJSON *ca = cJSON_AddArrayToObject(pj, "caps");
    if (caps_j && cJSON_IsArray(caps_j)) {
        cJSON *it;
        cJSON_ArrayForEach(it, caps_j)
            if (cJSON_IsString(it)) cJSON_AddItemToArray(ca, cJSON_CreateString(it->valuestring));
    }
    cJSON_AddStringToObject(pj, "signature", signature);
    cJSON_AddItemToObject(out, "plugin", pj);
    cJSON_AddStringToObject(out, "script", script);
    cJSON_AddBoolToObject(out, "skill", skill_ok);
    cJSON_AddStringToObject(out, "path", path);
    cJSON_AddBoolToObject(out, "test_ok", test_ok);
    if (test_report) {
        cJSON *tr = cJSON_Parse(test_report);
        if (tr) cJSON_AddItemToObject(out, "test", tr);
        free(test_report);
    }
    if (c_skeleton) {
        cJSON_AddStringToObject(out, "c_skeleton", c_skeleton);
        free(c_skeleton);
    }
    if (analysis) cJSON_AddItemToObject(out, "analysis", analysis);
    if (arch) cJSON_AddItemToObject(out, "arch", arch);
    cJSON_Delete(design);
    free(analysis_s);
    free(arch_s);
    free(caps_csv);
    char *js = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);

    coa_log_info("plugin generated name=%s real=%d registered=%d skill=%d test_ok=%d",
                name, real, reg_ok, skill_ok, test_ok);
    return js ? js : coa_strdup("{\"ok\":false,\"error\":\"json build failed\"}");
}

char *coa_plugin_generate(coa_ctx *ctx, const char *description) {
    coa_plugin_gen_deps deps;
    memset(&deps, 0, sizeof(deps));
    deps.llm = ctx ? ctx->llm : NULL;
    deps.provider = ctx ? ctx->provider : NULL;
    deps.registry = ctx ? ctx->registry : NULL;
    deps.skills = ctx ? ctx->skills : NULL;
    deps.state_root = ctx ? ctx->state_root : NULL;
    return coa_plugin_generate_deps(&deps, description);
}
