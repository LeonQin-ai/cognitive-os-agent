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
    if (!s)
        return 0;
    for (; *s; s++)
        if (*s == '"' || *s == '`' || *s == '\n' || *s == '\r' || (s[0] == '$' && s[1] == '('))
            return 1;
    return 0;
}

static tool_result *git_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *sub;
    const char *dir = NULL;
    cJSON *dir_j;
    char cmd[4096];
    proc_result *pr;
    tool_result *r;

    (void)self;
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "git: invalid args JSON");
    sub = cJSON_GetObjectItemCaseSensitive(args, "args");
    const char *subargs = (sub && cJSON_IsString(sub)) ? sub->valuestring : "";
    dir_j = cJSON_GetObjectItemCaseSensitive(args, "dir");
    if (dir_j && cJSON_IsString(dir_j))
        dir = dir_j->valuestring;

    if (has_shell_metachars(subargs) || has_shell_metachars(dir)) {
        cJSON_Delete(args);
        return tool_result_new(0, "git: args contain forbidden characters (quote/backtick/$(/newline)");
    }

    if (dir && *dir)
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s", dir, subargs);
    else if (ctx && ctx->workspace && *ctx->workspace)
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s", ctx->workspace, subargs);
    else
        snprintf(cmd, sizeof(cmd), "git %s", subargs);

    pr = proc_run(cmd, 15000);
    cJSON_Delete(args);
    if (!pr)
        return tool_result_new(0, "git: failed to spawn git");

    r = tool_result_new(pr->exit_code == 0 && !pr->timed_out, pr->output ? pr->output : "");
    proc_result_free(pr);
    return r;
}

const tool *tool_git(void) {
    static const tool t = {
        "git",
        "Run a git subcommand (e.g. args=\"status\" or args=\"log --oneline -5\").",
        "{\"type\":\"object\",\"properties\":{\"args\":{\"type\":\"string\"},"
        "\"dir\":{\"type\":\"string\",\"description\":\"optional working directory - "
        "OMIT this property unless the user explicitly names one\"}}}",
        1,
        git_exec,
    };
    return &t;
}
