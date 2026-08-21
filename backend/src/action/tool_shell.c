/* tool_shell.c — shell command execution tool. */
#include "cagent/action/tools.h"
#include "cagent/os/os_proc.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

static ca_tool_result *shell_exec(const ca_tool_ctx *ctx, const char *args_json) {
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return ca_tool_result_new(0, "shell: invalid args JSON");
    cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd_j || !cJSON_IsString(cmd_j)) {
        cJSON_Delete(args);
        return ca_tool_result_new(0, "shell: missing string arg 'command'");
    }
    int timeout_ms = 15000;
    cJSON *t_j = cJSON_GetObjectItemCaseSensitive(args, "timeout_ms");
    if (t_j && cJSON_IsNumber(t_j)) timeout_ms = (int)t_j->valuedouble;

    ca_proc_result *pr = ca_proc_run_in(cmd_j->valuestring, timeout_ms,
                                        ctx ? ctx->workspace : NULL);
    cJSON_Delete(args);
    if (!pr) return ca_tool_result_new(0, "shell: failed to spawn process");

    ca_tool_result *r;
    if (pr->timed_out) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "[timeout] %s\n%s", pr->output ? pr->output : "", "command exceeded time limit");
        r = ca_tool_result_new(0, msg);
    } else if (pr->exit_code != 0) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "exit code %d\n%s", pr->exit_code, pr->output ? pr->output : "");
        r = ca_tool_result_new(0, msg);
    } else {
        r = ca_tool_result_new(1, pr->output ? pr->output : "");
    }
    ca_proc_result_free(pr);
    return r;
}

const ca_tool *ca_tool_shell(void) {
    static const ca_tool t = {
        "shell",
        "Run a shell command and capture combined stdout+stderr.",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\"}}}",
        1,
        shell_exec,
    };
    return &t;
}
