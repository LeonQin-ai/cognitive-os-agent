/* tool_shell.c — shell command execution tool. */
#include "action/tools.h"
#include "os/os_proc.h"
#include "infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"

#if defined(_WIN32)
#include <windows.h>
/* Child processes on Chinese Windows emit GBK/OEM text. Convert to UTF-8 so
 * the output is readable AND valid for the LLM API (which hard-rejects
 * invalid UTF-8). Returns a malloc'd string or NULL. */
static char *oem_to_utf8(const char *in) {
    int wlen = MultiByteToWideChar(CP_OEMCP, 0, in, -1, NULL, 0);
    int u8len;
    char *u8;

    if (wlen <= 0)
        return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w)
        return NULL;
    if (MultiByteToWideChar(CP_OEMCP, 0, in, -1, w, wlen) <= 0) {
        free(w);
        return NULL;
    }

    u8len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (u8len <= 0) {
        free(w);
        return NULL;
    }

    u8 = (char *)malloc((size_t)u8len);
    if (!u8) {
        free(w);
        return NULL;
    }

    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, u8len, NULL, NULL) <= 0) {
        free(u8);
        u8 = NULL;
    }

    free(w);
    return u8;
}
#endif

static tool_result *shell_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args;
    cJSON *cmd_j;
    int timeout_ms = 15000;
    cJSON *t_j;
    proc_result *pr;
    char *converted = NULL;
    tool_result *r;

    (void)self;
    args = cJSON_Parse(args_json);
    if (!args)
        return tool_result_new(0, "shell: invalid args JSON");
    cmd_j = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cmd_j || !cJSON_IsString(cmd_j)) {
        cJSON_Delete(args);
        return tool_result_new(0, "shell: missing string arg 'command'");
    }

    t_j = cJSON_GetObjectItemCaseSensitive(args, "timeout_ms");
    if (t_j && cJSON_IsNumber(t_j))
        timeout_ms = (int)t_j->valuedouble;
    if (timeout_ms < 100)
        timeout_ms = 100;
    if (timeout_ms > 60000)
        timeout_ms = 60000;

    pr = proc_run_in(cmd_j->valuestring, timeout_ms, ctx ? ctx->workspace : NULL);
    cJSON_Delete(args);
    if (!pr)
        return tool_result_new(0, "shell: failed to spawn process");

    /* Normalize output encoding: prefer the OEM->UTF-8 conversion on Windows
     * when the raw bytes are not valid UTF-8; last resort is lossy sanitize
     * so the context never carries invalid UTF-8. */
    const char *out_text = pr->output ? pr->output : "";
    if (*out_text && !str_utf8_valid_n(out_text, -1)) {
#if defined(_WIN32)
        converted = oem_to_utf8(out_text);
        if (converted && !str_utf8_valid_n(converted, -1)) {
            free(converted);
            converted = NULL;
        }
#endif
        if (!converted) {
            converted = str_utf8_sanitize(out_text);
        }
        if (converted)
            out_text = converted;
    }

    if (pr->timed_out) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "[timeout] %s\n%s", out_text, "command exceeded time limit");
        r = tool_result_new(0, msg);
    } else if (pr->exit_code != 0) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "exit code %d\n%s", pr->exit_code, out_text);
        r = tool_result_new(0, msg);
    } else {
        r = tool_result_new(1, out_text);
    }

    proc_result_free(pr);
    free(converted);
    return r;
}

const tool *tool_shell(void) {
    static const tool t = {
        "shell",
        "Run a shell command and capture combined stdout+stderr.",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\"}}"
        "}",
        1,
        shell_exec,
        NULL,
    };
    return &t;
}

static int ssh_host_valid(const char *s) {
    if (!s || !*s)
        return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!isalnum(c) && c != '.' && c != '-' && c != '_' && c != '@' && c != ':' && c != '[' && c != ']')
            return 0;
    }
    return 1;
}

static char *ssh_quote_command(const char *s) {
    strbuf out;
    strbuf_init(&out);
    strbuf_append(&out, "\"");
    for (; s && *s; s++) {
        if (*s == '\r' || *s == '\n') {
            strbuf_free(&out);
            return NULL;
        }
        if (*s == '\\' || *s == '\"')
            strbuf_append(&out, "\\");
        char one[2] = {*s, '\0'};
        strbuf_append(&out, one);
    }
    strbuf_append(&out, "\"");
    return strbuf_detach(&out);
}

static tool_result *ssh_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args = cJSON_Parse(args_json);
    cJSON *host = args ? cJSON_GetObjectItemCaseSensitive(args, "host") : NULL;
    cJSON *command = args ? cJSON_GetObjectItemCaseSensitive(args, "command") : NULL;
    cJSON *port = args ? cJSON_GetObjectItemCaseSensitive(args, "port") : NULL;
    cJSON *timeout = args ? cJSON_GetObjectItemCaseSensitive(args, "timeout_ms") : NULL;
    int timeout_ms = timeout && cJSON_IsNumber(timeout) ? (int)timeout->valuedouble : 15000;
    int port_no = port && cJSON_IsNumber(port) ? (int)port->valuedouble : 22;
    char *quoted = NULL;
    char *shell_args = NULL;
    tool_result *result;
    strbuf cmd;
    (void)self;

    if (!host || !cJSON_IsString(host) || !ssh_host_valid(host->valuestring) ||
        !command || !cJSON_IsString(command) || port_no < 1 || port_no > 65535) {
        cJSON_Delete(args);
        return tool_result_new(0, "ssh: host, command or port is invalid");
    }
    quoted = ssh_quote_command(command->valuestring);
    if (!quoted) {
        cJSON_Delete(args);
        return tool_result_new(0, "ssh: command must be a single line");
    }
    if (timeout_ms < 100)
        timeout_ms = 100;
    if (timeout_ms > 60000)
        timeout_ms = 60000;
    strbuf_init(&cmd);
    strbuf_appendf(&cmd, "ssh -o BatchMode=yes -o ConnectTimeout=5 -p %d -- %s %s", port_no,
                   host->valuestring, quoted);
    cJSON *wrapped = cJSON_CreateObject();
    cJSON_AddStringToObject(wrapped, "command", cmd.buf);
    cJSON_AddNumberToObject(wrapped, "timeout_ms", timeout_ms);
    shell_args = cJSON_PrintUnformatted(wrapped);
    cJSON_Delete(wrapped);
    strbuf_free(&cmd);
    free(quoted);
    cJSON_Delete(args);
    if (!shell_args)
        return tool_result_new(0, "ssh: out of memory");
    result = shell_exec(NULL, ctx, shell_args);
    free(shell_args);
    return result;
}

const tool *tool_ssh(void) {
    static const tool t = {
        "ssh",
        "Run one non-interactive command on an SSH host using keys or an SSH agent.",
        "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\"},"
        "\"command\":{\"type\":\"string\"},\"port\":{\"type\":\"integer\"},"
        "\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"host\",\"command\"]}",
        1,
        ssh_exec,
        NULL,
    };
    return &t;
}
