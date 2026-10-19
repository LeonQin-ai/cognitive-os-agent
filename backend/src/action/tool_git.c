/* tool_git.c — git wrapper tool (invokes the git CLI). */
#include "action/tools.h"
#include "os/os_proc.h"
#include "infra/util.h"

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
    /* Models sometimes emit git {} while checking the repository. Treat that
     * read-only invocation as status rather than invoking bare git help. */
    const char *subargs = (sub && cJSON_IsString(sub) && sub->valuestring[0])
                              ? sub->valuestring : "status --short";
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

    if (pr->exit_code != 0 && !pr->timed_out && pr->output &&
        strstr(pr->output, "not a git repository")) {
        char diagnostic[8192];
        const char *working_dir = (dir && *dir) ? dir :
                                  (ctx && ctx->workspace && *ctx->workspace) ? ctx->workspace : ".";
        snprintf(diagnostic, sizeof(diagnostic),
                 "git: workspace '%s' is not inside a Git repository. "
                 "The desktop app runs tools from its configured workspace (default '.' relative to the app directory), "
                 "not from the source checkout automatically. Set the workspace to the repository root or pass its path "
                 "in the git tool's 'dir' property. Original error: %.6000s",
                 working_dir, pr->output);
        r = tool_result_new(0, diagnostic);
        proc_result_free(pr);
        return r;
    }

    r = tool_result_new(pr->exit_code == 0 && !pr->timed_out, pr->output ? pr->output : "");
    proc_result_free(pr);
    return r;
}

const tool *tool_git(void) {
    static const tool t = {
        "git",
        "Run a git subcommand (e.g. args=\"status\" or args=\"log --oneline -5\"). "
        "Uses the configured workspace by default. If that is not a Git repository, pass dir with a repository path "
        "identified from the task context.",
        "{\"type\":\"object\",\"properties\":{\"args\":{\"type\":\"string\"},"
        "\"dir\":{\"type\":\"string\",\"description\":\"optional working directory - "
        "use the repository path when it differs from the configured workspace\"}}}",
        1,
        git_exec,
        NULL,
    };
    return &t;
}
