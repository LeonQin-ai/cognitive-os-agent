/* testing.c — plugin test planning + smoke execution. */
#include "cognitive-os-agent/plugin_intelligence/testing.h"
#include "cognitive-os-agent/plugin_runtime/sandbox.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

char *coa_testing_plan(const char *spec_json) {
    cJSON *root = cJSON_Parse(spec_json ? spec_json : "{}");
    cJSON *name = root ? cJSON_GetObjectItemCaseSensitive(root, "name") : NULL;
    const char *n = name && cJSON_IsString(name) ? name->valuestring : "plugin";

    cJSON *out = cJSON_CreateObject();
    if (out) {
        cJSON_AddStringToObject(out, "name", n);
        cJSON *cases = cJSON_CreateArray();
        cJSON *c1 = cJSON_CreateObject();
        cJSON_AddStringToObject(c1, "id", "smoke");
        cJSON_AddStringToObject(c1, "desc", "plugin loads and returns ok");
        cJSON_AddItemToArray(cases, c1);
        cJSON *c2 = cJSON_CreateObject();
        cJSON_AddStringToObject(c2, "id", "empty_args");
        cJSON_AddStringToObject(c2, "desc", "empty args are handled");
        cJSON_AddItemToArray(cases, c2);
        cJSON_AddItemToObject(out, "cases", cases);
    }
    if (root) cJSON_Delete(root);
    char *s = out ? cJSON_PrintUnformatted(out) : NULL;
    if (out) cJSON_Delete(out);
    return s ? s : coa_strdup("{}");
}

char *coa_testing_run(const char *cmd, int timeout_ms) {
    coa_sandbox *sb = coa_sandbox_new(timeout_ms);
    /* track file accesses of the tested command (cwd scan + cmd reads) */
    coa_sandbox_set_workspace(sb, ".");
    coa_sandbox_result *r = coa_sandbox_run(sb, cmd);
    coa_sandbox_free(sb);

    cJSON *out = cJSON_CreateObject();
    if (out) {
        if (r) {
            cJSON_AddBoolToObject(out, "ok", r->ok);
            cJSON_AddNumberToObject(out, "exit_code", r->exit_code);
            cJSON_AddNumberToObject(out, "timed_out", r->timed_out);
            cJSON_AddStringToObject(out, "output", r->output ? r->output : "");
            if (r->files_json) {
                cJSON *fj = cJSON_Parse(r->files_json);
                if (fj) cJSON_AddItemToObject(out, "files", fj);
                else cJSON_AddStringToObject(out, "files", r->files_json);
            }
        } else {
            cJSON_AddBoolToObject(out, "ok", 0);
            cJSON_AddNumberToObject(out, "exit_code", -1);
            cJSON_AddNumberToObject(out, "timed_out", 0);
            cJSON_AddStringToObject(out, "output", "(forbidden or spawn failed)");
        }
    }
    if (r) coa_sandbox_result_free(r);
    char *s = out ? cJSON_PrintUnformatted(out) : NULL;
    if (out) cJSON_Delete(out);
    return s ? s : coa_strdup("{}");
}
