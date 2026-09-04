/* tool_skill.c — run a registered skill by name.
 * Bridges the planner-facing tool registry to the skill registry so the LLM
 * can invoke named Shell/Python skills (list_dir, sys_info, ...) it sees in
 * its plan prompt. Unknown names return the available list so the model can
 * self-correct on the next turn. */
#include "cagent/action/tools.h"
#include "cagent/action/skill.h"
#include "cagent/infra/util.h"
#include "cagent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define SKILL_TIMEOUT_MS 30000

static ca_tool_result *skill_exec(const ca_tool *self, const ca_tool_ctx *ctx, const char *args_json) {
    (void)self;
    if (!ctx || !ctx->skills)
        return ca_tool_result_new(0, "skill: no skill registry available");
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return ca_tool_result_new(0, "skill: invalid args JSON");
    cJSON *name_j = cJSON_GetObjectItemCaseSensitive(args, "name");
    if (!name_j || !cJSON_IsString(name_j)) {
        cJSON_Delete(args);
        return ca_tool_result_new(0, "skill: missing string arg 'name'");
    }
    const char *name = name_j->valuestring;

    if (!ca_skill_find(ctx->skills, name)) {
        char *list = ca_skill_list_json(ctx->skills);
        char msg[1024];
        snprintf(msg, sizeof msg, "skill: unknown skill '%s'. available: %s",
                 name, list ? list : "[]");
        free(list);
        cJSON_Delete(args);
        return ca_tool_result_new(0, msg);
    }

    /* Pass the optional args object through for {{placeholder}} binding. */
    char *args_out = NULL;
    cJSON *a_j = cJSON_GetObjectItemCaseSensitive(args, "args");
    if (a_j && cJSON_IsObject(a_j)) args_out = cJSON_PrintUnformatted(a_j);
    ca_skill_result *r = ca_skill_execute(ctx->skills, name, args_out ? args_out : "{}",
                                          ctx->workspace, SKILL_TIMEOUT_MS);
    free(args_out);
    cJSON_Delete(args);
    if (!r)
        return ca_tool_result_new(0, "skill: execution rejected (sandbox/policy)");
    ca_tool_result *tr = ca_tool_result_new(r->ok ? 1 : 0,
                                            r->output ? r->output : "");
    ca_skill_result_free(r);
    return tr;
}

const ca_tool *ca_tool_skill(void) {
    static const ca_tool t = {
        "skill",
        "Execute a REGISTERED SKILL by name (available skills are listed in the plan prompt). "
        "Skills whose body contains {{placeholders}} accept an 'args' object.",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
        "\"args\":{\"type\":\"object\",\"description\":\"parameter values for {{placeholders}} in the skill body\"}},"
        "\"required\":[\"name\"]}",
        1,
        skill_exec,
    };
    return &t;
}

/* ---- dynamic tools for generated (self-evolved) plugins ----
 * When the reasoning loop misses a capability it auto-generates a plugin
 * (skill) and binds it under the planned tool name so the running task can
 * proceed. Mirrors the dynamic MCP tool registration pattern. */
typedef struct generated_tool_ud {
    struct ca_skill_registry *skills;
    char *skill_name;
} generated_tool_ud;

static ca_tool_result *generated_tool_exec(const ca_tool *self, const ca_tool_ctx *ctx,
                                           const char *args_json) {
    (void)self;
    generated_tool_ud *ud = self ? (generated_tool_ud *)self->ud : NULL;
    if (!ud || !ud->skills)
        return ca_tool_result_new(0, "generated tool: broken binding");
    ca_skill_result *r = ca_skill_execute(ud->skills, ud->skill_name,
                                          args_json ? args_json : "{}",
                                          ctx ? ctx->workspace : NULL,
                                          SKILL_TIMEOUT_MS);
    if (!r)
        return ca_tool_result_new(0, "generated tool: execution rejected (sandbox/policy)");
    ca_tool_result *tr = ca_tool_result_new(r->ok ? 1 : 0, r->output ? r->output : "");
    ca_skill_result_free(r);
    return tr;
}

int ca_tool_register_generated(ca_tool_registry *reg, struct ca_skill_registry *skills,
                               const char *tool_name, const char *skill_name) {
    if (!reg || !skills || !tool_name || !skill_name) return -1;
    if (!ca_skill_find(skills, skill_name)) return -1; /* skill must exist */
    if (ca_tool_find(reg, tool_name)) return 0; /* already present */
    ca_tool *t = (ca_tool *)calloc(1, sizeof(*t));
    generated_tool_ud *ud = (generated_tool_ud *)calloc(1, sizeof(*ud));
    if (!t || !ud) { free(t); free(ud); return -1; }
    ud->skills = skills;
    ud->skill_name = ca_strdup(skill_name);
    t->name = ca_strdup(tool_name);
    char desc[512];
    snprintf(desc, sizeof(desc),
             "[generated plugin] capability auto-created at runtime (skill: %s)",
             skill_name);
    t->description = ca_strdup(desc);
    t->json_schema = NULL;
    t->is_write = 1;
    t->execute = generated_tool_exec;
    t->ud = ud;
    if (ca_tool_register_ex(reg, t, 0) != 0) return -1;
    return 0;
}

/* ---- generated tool <-> skill mapping (self-evolution persistence) ----
 * The in-process binding dies with the process; this file maps tool names to
 * skills under <state_root>/generated_tools.json so cagent_init can re-bind
 * them at startup. */

int ca_tool_generated_save_mapping(const char *state_root, const char *tool,
                                   const char *skill) {
    if (!state_root || !*state_root || !tool || !*tool || !skill || !*skill) return -1;
    char path[600];
    snprintf(path, sizeof(path), "%s/generated_tools.json", state_root);
    cJSON *arr = NULL;
    char *old = ca_fs_read_file(path);
    if (old) { arr = cJSON_Parse(old); free(old); }
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        arr = cJSON_CreateArray();
    }
    /* upsert: an entry bound to the same tool name is replaced */
    int idx = 0, found = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "tool");
        if (t && cJSON_IsString(t) && strcmp(t->valuestring, tool) == 0) { found = 1; break; }
        idx++;
    }
    if (found) cJSON_DeleteItemFromArray(arr, idx);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "tool", tool);
    cJSON_AddStringToObject(e, "skill", skill);
    cJSON_AddItemToArray(arr, e);
    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!js) return -1;
    int rc = ca_fs_write_file(path, js, strlen(js));
    free(js);
    return rc == 0 ? 0 : -1;
}

char *ca_tool_generated_load_mapping(const char *state_root) {
    if (!state_root || !*state_root) return NULL;
    char path[600];
    snprintf(path, sizeof(path), "%s/generated_tools.json", state_root);
    return ca_fs_read_file(path);
}
