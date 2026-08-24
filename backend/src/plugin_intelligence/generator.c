/* generator.c — AI plugin generation pipeline (self-evolution loop). */
#include "cagent/plugin_intelligence/generator.h"
#include "cagent/plugin_intelligence/security.h"
#include "cagent/plugin_runtime/sandbox.h"
#include "cagent/plugin_runtime/registry.h"
#include "cagent/action/skill.h"
#include "cagent/llm/llm.h"
#include "cagent/os/os_fs.h"
#include "cagent/infra/util.h"
#include "cagent/infra/logging.h"
#include "cagent/cagent.h"

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
    if (!script || ca_sandbox_forbidden(script)) return 0;
    char *audit = ca_security_audit(script);
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

char *ca_plugin_generate(cagent_ctx *ctx, const char *description) {
    if (!ctx || !description || !*description)
        return ca_strdup("{\"ok\":false,\"error\":\"missing description\"}");

    cJSON *design = NULL;
    const int real = (ctx->provider && strcmp(ctx->provider, "mock") != 0);

    if (real && ctx->llm) {
        char *resp = ca_llm_chat_simple(ctx->llm, ARCH_PROMPT, description);
        design = parse_design(resp);
        free(resp);
    }
    if (!design) {
        /* fallback: mock template (also used when the LLM returns garbage) */
        char *js = mock_design(description);
        design = js ? cJSON_Parse(js) : NULL;
        free(js);
        if (!design)
            return ca_strdup("{\"ok\":false,\"error\":\"pipeline failed\"}");
    }

    char name[64], script[65536];
    if (!design_ok(design, name, sizeof(name), script, sizeof(script))) {
        cJSON_Delete(design);
        return ca_strdup("{\"ok\":false,\"error\":\"LLM design invalid (missing name/script)\"}");
    }

    cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(design, "description");
    const char *desc = desc_j && cJSON_IsString(desc_j) ? desc_j->valuestring : description;
    cJSON *caps_j = cJSON_GetObjectItemCaseSensitive(design, "capabilities");

    /* --- security review gate --- */
    if (!security_ok(script)) {
        cJSON_Delete(design);
        return ca_strdup("{\"ok\":false,\"error\":\"security review rejected the generated script\"}");
    }

    /* --- content signature --- */
    uint64_t h = ca_hash64(script, strlen(script));
    char signature[32];
    ca_hash_hex(signature, h);

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
    if (ctx->registry) {
        ca_plugin_meta meta;
        memset(&meta, 0, sizeof(meta));
        meta.name = name;
        meta.version = (char *)PLUGIN_VERSION;
        meta.signature = signature;
        meta.description = (char *)desc;
        meta.caps = caps;
        meta.n_caps = n_caps;
        meta.enabled = 1;
        meta.built_ms = 0;
        reg_ok = ca_plugin_registry_register(ctx->registry, &meta) == 0;
    }
    free(caps);

    /* --- register as a runnable skill --- */
    int skill_ok = 0;
    if (ctx->skills) {
        const ca_skill sk = { name, desc, "shell", script };
        skill_ok = ca_skill_register(ctx->skills, &sk) == 0;
    }

    /* --- persist under <state_root>/plugins/<name>.sh --- */
    char dir[600], path[700];
    snprintf(dir, sizeof(dir), "%s", ctx->state_root ? ctx->state_root : "state");
    snprintf(path, sizeof(path), "%s/plugins", dir);
    ca_fs_mkdirs(path);
    snprintf(path, sizeof(path), "%s/plugins/%s.sh", dir, name);
    ca_fs_write_file(path, script, (size_t)strlen(script));

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", 1);
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", name);
    cJSON_AddStringToObject(p, "version", PLUGIN_VERSION);
    cJSON_AddStringToObject(p, "description", desc);
    cJSON *ca = cJSON_AddArrayToObject(p, "caps");
    if (caps_j && cJSON_IsArray(caps_j)) {
        cJSON *it;
        cJSON_ArrayForEach(it, caps_j)
            if (cJSON_IsString(it)) cJSON_AddItemToArray(ca, cJSON_CreateString(it->valuestring));
    }
    cJSON_AddStringToObject(p, "signature", signature);
    cJSON_AddItemToObject(out, "plugin", p);
    cJSON_AddStringToObject(out, "script", script);
    cJSON_AddBoolToObject(out, "skill", skill_ok);
    cJSON_AddStringToObject(out, "path", path);
    cJSON_Delete(design);
    char *js = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);

    ca_log_info("plugin generated name=%s real=%d registered=%d skill=%d", name, real, reg_ok, skill_ok);
    return js ? js : ca_strdup("{\"ok\":false,\"error\":\"json build failed\"}");
}
