/* tool_shell.c — shell command execution tool. */
#include "cognitive-os-agent/action/tools.h"
#include "cognitive-os-agent/os/os_proc.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#if defined(_WIN32)
#include <windows.h>
/* Child processes on Chinese Windows emit GBK/OEM text. Convert to UTF-8 so
 * the output is readable AND valid for the LLM API (which hard-rejects
 * invalid UTF-8). Returns a malloc'd string or NULL. */
static char *oem_to_utf8(const char *in) {
    int wlen = MultiByteToWideChar(CP_OEMCP, 0, in, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_OEMCP, 0, in, -1, w, wlen) <= 0) {
        free(w);
        return NULL;
    }
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (u8len <= 0) { free(w); return NULL; }
    char *u8 = (char *)malloc((size_t)u8len);
    if (!u8) { free(w); return NULL; }
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, u8len, NULL, NULL) <= 0) {
        free(u8); u8 = NULL;
    }
    free(w);
    return u8;
}
#endif

static coa_tool_result *shell_exec(const coa_tool *self, const coa_tool_ctx *ctx, const char *args_json) {
    (void)self;
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return coa_tool_result_new(0, "shell: invalid args JSON");
    cJSON *cmd_j = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd_j || !cJSON_IsString(cmd_j)) {
        cJSON_Delete(args);
        return coa_tool_result_new(0, "shell: missing string arg 'command'");
    }
    int timeout_ms = 15000;
    cJSON *t_j = cJSON_GetObjectItemCaseSensitive(args, "timeout_ms");
    if (t_j && cJSON_IsNumber(t_j)) timeout_ms = (int)t_j->valuedouble;

    coa_proc_result *pr = coa_proc_run_in(cmd_j->valuestring, timeout_ms,
                                        ctx ? ctx->workspace : NULL);
    cJSON_Delete(args);
    if (!pr) return coa_tool_result_new(0, "shell: failed to spawn process");

    /* Normalize output encoding: prefer the OEM->UTF-8 conversion on Windows
     * when the raw bytes are not valid UTF-8; last resort is lossy sanitize
     * so the context never carries invalid UTF-8. */
    char *converted = NULL;
    const char *out_text = pr->output ? pr->output : "";
    if (*out_text && !coa_str_utf8_valid_n(out_text, -1)) {
#if defined(_WIN32)
        converted = oem_to_utf8(out_text);
        if (converted && !coa_str_utf8_valid_n(converted, -1)) {
            free(converted);
            converted = NULL;
        }
#endif
        if (!converted) {
            converted = coa_str_utf8_sanitize(out_text);
        }
        if (converted) out_text = converted;
    }

    coa_tool_result *r;
    if (pr->timed_out) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "[timeout] %s\n%s", out_text, "command exceeded time limit");
        r = coa_tool_result_new(0, msg);
    } else if (pr->exit_code != 0) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "exit code %d\n%s", pr->exit_code, out_text);
        r = coa_tool_result_new(0, msg);
    } else {
        r = coa_tool_result_new(1, out_text);
    }
    coa_proc_result_free(pr);
    free(converted);
    return r;
}

const coa_tool *coa_tool_shell(void) {
    static const coa_tool t = {
        "shell",
        "Run a shell command and capture combined stdout+stderr.",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\"}}}",
        1,
        shell_exec,
    };
    return &t;
}
