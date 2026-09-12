/* tool_skill.c — run a registered skill by name.
 * Bridges the planner-facing tool registry to the skill registry so the LLM
 * can invoke named Shell/Python skills (list_dir, sys_info, ...) it sees in
 * its plan prompt. Unknown names return the available list so the model can
 * self-correct on the next turn. */
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/action/skill.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define SKILL_TIMEOUT_MS 30000

static tool_result *skill_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *name_j;
    /* Pass the optional args object through for {{placeholder}} binding. */
    char *args_out = NULL;
    cJSON *a_j;
    skill_result *r;
    tool_result *tr;

    (void)self;
    if (!ctx || !ctx->skills)
        return tool_result_new(0, "skill: no skill registry available");
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "skill: invalid args JSON");
    name_j = cJSON_GetObjectItemCaseSensitive(args, "name");
    if (!name_j || !cJSON_IsString(name_j)) {
        cJSON_Delete(args);
        return tool_result_new(0, "skill: missing string arg 'name'");
    }

    const char *name = name_j->valuestring;

    if (!skill_find(ctx->skills, name)) {
        char *list = skill_list_json(ctx->skills);
        char msg[1024];
        snprintf(msg, sizeof msg, "skill: unknown skill '%s'. available: %s", name, list ? list : "[]");
        free(list);
        cJSON_Delete(args);
        return tool_result_new(0, msg);
    }

    a_j = cJSON_GetObjectItemCaseSensitive(args, "args");
    if (a_j && cJSON_IsObject(a_j))
        args_out = cJSON_PrintUnformatted(a_j);
    r =
        skill_execute(ctx->skills, name, args_out ? args_out : "{}", ctx->workspace, SKILL_TIMEOUT_MS);
    free(args_out);
    cJSON_Delete(args);
    if (!r)
        return tool_result_new(0, "skill: execution rejected (sandbox/policy)");
    tr = tool_result_new(r->ok ? 1 : 0, r->output ? r->output : "");
    skill_result_free(r);
    return tr;
}

const tool *tool_skill(void) {
    static const tool t = {
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
    struct skill_registry *skills;
    char *skill_name;
} generated_tool_ud;

static tool_result *generated_tool_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    tool_result *tr;

    (void)self;
    generated_tool_ud *ud = self ? (generated_tool_ud *)self->ud : NULL;
    if (!ud || !ud->skills)
        return tool_result_new(0, "generated tool: broken binding");
    skill_result *r = skill_execute(ud->skills, ud->skill_name, args_json ? args_json : "{}",
                                            ctx ? ctx->workspace : NULL, SKILL_TIMEOUT_MS);
    if (!r)
        return tool_result_new(0, "generated tool: execution rejected (sandbox/policy)");
    tr = tool_result_new(r->ok ? 1 : 0, r->output ? r->output : "");
    skill_result_free(r);
    return tr;
}

int tool_register_generated(tool_registry *reg, struct skill_registry *skills, const char *tool_name,
                                const char *skill_name) {
    tool *t;
    char desc[512];

    if (!reg || !skills || !tool_name || !skill_name)
        return -1;
    if (!skill_find(skills, skill_name))
        return -1; /* skill must exist */
    if (tool_find(reg, tool_name))
        return 0; /* already present */
    t = (tool *)calloc(1, sizeof(*t));
    generated_tool_ud *ud = (generated_tool_ud *)calloc(1, sizeof(*ud));
    if (!t || !ud) {
        free(t);
        free(ud);
        return -1;
    }

    ud->skills = skills;
    ud->skill_name = xstrdup(skill_name);
    t->name = xstrdup(tool_name);
    snprintf(desc, sizeof(desc), "[generated plugin] capability auto-created at runtime (skill: %s)", skill_name);
    t->description = xstrdup(desc);
    t->json_schema = NULL;
    t->is_write = 1;
    t->execute = generated_tool_exec;
    t->ud = ud;
    if (tool_register_ex(reg, t, 0) != 0)
        return -1;
    return 0;
}

/* ---- generated tool <-> skill mapping (self-evolution persistence) ----
 * The in-process binding dies with the process; this file maps tool names to
 * skills under <state_root>/generated_tools.json so init can re-bind
 * them at startup. */

int tool_generated_save_mapping(const char *state_root, const char *tool, const char *skill) {
    char path[600];
    cJSON *arr = NULL;
    char *old;
    cJSON *it;
    cJSON *e;
    char *js;
    int rc;

    if (!state_root || !*state_root || !tool || !*tool || !skill || !*skill)
        return -1;
    snprintf(path, sizeof(path), "%s/generated_tools.json", state_root);
    old = fs_read_file(path);
    if (old) {
        arr = cJSON_Parse(old);
        free(old);
    }

    if (!arr || !cJSON_IsArray(arr)) {
        if (arr)
            cJSON_Delete(arr);
        arr = cJSON_CreateArray();
    }

    /* upsert: an entry bound to the same tool name is replaced */
    int idx = 0, found = 0;
    cJSON_ArrayForEach(it, arr) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "tool");
        if (t && cJSON_IsString(t) && strcmp(t->valuestring, tool) == 0) {
            found = 1;
            break;
        }

        idx++;
    }

    if (found)
        cJSON_DeleteItemFromArray(arr, idx);
    e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "tool", tool);
    cJSON_AddStringToObject(e, "skill", skill);
    cJSON_AddItemToArray(arr, e);
    js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!js)
        return -1;
    rc = fs_write_file(path, js, strlen(js));
    free(js);
    return rc == 0 ? 0 : -1;
}

char *tool_generated_load_mapping(const char *state_root) {
    char path[600];

    if (!state_root || !*state_root)
        return NULL;
    snprintf(path, sizeof(path), "%s/generated_tools.json", state_root);
    return fs_read_file(path);
}
