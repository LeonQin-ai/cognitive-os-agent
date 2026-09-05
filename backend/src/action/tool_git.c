/* tool_git.c — git wrapper tool (invokes the git CLI). */
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/os/os_proc.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

/* Reject inputs that could escape the quoted argument context. */
static int has_shell_metachars(const char *s) {
    if (!s) return 0;
    for (; *s; s++)
        if (*s == '"' || *s == '`' || *s == '\n' || *s == '\r' ||
            (s[0] == '$' && s[1] == '('))
            return 1;
    return 0;
}

static coa_tool_result *git_exec(const coa_tool *self, const coa_tool_ctx *ctx, const char *args_json) {
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return coa_tool_result_new(0, "git: invalid args JSON");
    cJSON *sub = cJSON_GetObjectItemCaseSensitive(args, "args");
    const char *subargs = (sub && cJSON_IsString(sub)) ? sub->valuestring : "";
    const char *dir = NULL;
    cJSON *dir_j = cJSON_GetObjectItemCaseSensitive(args, "dir");
    if (dir_j && cJSON_IsString(dir_j)) dir = dir_j->valuestring;

    if (has_shell_metachars(subargs) || has_shell_metachars(dir)) {
        cJSON_Delete(args);
        return coa_tool_result_new(0, "git: args contain forbidden characters (quote/backtick/$(/newline)");
    }

    char cmd[4096];
    if (dir && *dir)
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s", dir, subargs);
    else if (ctx && ctx->workspace && *ctx->workspace)
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s", ctx->workspace, subargs);
    else
        snprintf(cmd, sizeof(cmd), "git %s", subargs);

    coa_proc_result *pr = coa_proc_run(cmd, 15000);
    cJSON_Delete(args);
    if (!pr) return coa_tool_result_new(0, "git: failed to spawn git");

    coa_tool_result *r = coa_tool_result_new(pr->exit_code == 0 && !pr->timed_out,
                                           pr->output ? pr->output : "");
    coa_proc_result_free(pr);
    return r;
}

const coa_tool *coa_tool_git(void) {
    static const coa_tool t = {
        "git",
        "Run a git subcommand (e.g. args=\"status\" or args=\"log --oneline -5\").",
        "{\"type\":\"object\",\"properties\":{\"args\":{\"type\":\"string\"},\"dir\":{\"type\":\"string\"}}}",
        1,
        git_exec,
    };
    return &t;
}
