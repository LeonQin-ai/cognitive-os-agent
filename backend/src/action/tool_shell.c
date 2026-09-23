/* tool_shell.c — shell command execution tool. */
#include "action/tools.h"
#include "os/os_proc.h"
#include "os/os_fs.h"
#include "os/os_time.h"
#include "infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdatomic.h>
#include "cJSON.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

#if defined(_WIN32)
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

static tool_result *shell_exec_impl(const tool *self, const tool_ctx *ctx, const char *args_json,
                                    int native_shell) {
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

    pr = native_shell
        ? proc_run_native_in(cmd_j->valuestring, timeout_ms, ctx ? ctx->workspace : NULL)
        : proc_run_in(cmd_j->valuestring, timeout_ms, ctx ? ctx->workspace : NULL);
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

static tool_result *shell_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    return shell_exec_impl(self, ctx, args_json, 0);
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

static int ssh_name_valid(const char *s) {
    if (!s || !*s)
        return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!isalnum(c) && c != '_' && c != '-' && c != '.')
            return 0;
    }
    return 1;
}

/* Profile paths are local configuration, but still validate them before
 * inserting them into a shell command. Paths with spaces are supported. */
static int ssh_path_valid(const char *s) {
    if (!s || !*s)
        return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!isalnum(c) && !strchr("._-/\\: ", c))
            return 0;
    }
    return 1;
}

/* Quote one local-shell argument. The remote command remains exactly one ssh
 * argument, so metacharacters are interpreted remotely rather than locally. */
static char *ssh_quote_arg(const char *s) {
    strbuf out;
    strbuf_init(&out);
#if defined(_WIN32)
    strbuf_append(&out, "\"");
    for (; s && *s; s++) {
        if (*s == '\r' || *s == '\n') {
            strbuf_free(&out);
            return NULL;
        }
        if (*s == '\\' || *s == '\"')
            strbuf_append(&out, "\\");
        if (strchr("^&|<>()%!", *s))
            strbuf_append(&out, "^");
        char one[2] = {*s, '\0'};
        strbuf_append(&out, one);
    }
    strbuf_append(&out, "\"");
#else
    strbuf_append(&out, "'");
    for (; s && *s; s++) {
        if (*s == '\r' || *s == '\n') {
            strbuf_free(&out);
            return NULL;
        }
        if (*s == '\'')
            strbuf_append(&out, "'\\''");
        else {
            char one[2] = {*s, '\0'};
            strbuf_append(&out, one);
        }
    }
    strbuf_append(&out, "'");
#endif
    return strbuf_detach(&out);
}

typedef struct ssh_profile {
    const char *host, *user, *identity_file, *proxy_jump, *known_hosts;
    int port;
} ssh_profile;

/* Read a named SSH environment from <state>/ssh/environments.json:
 * {"environments":{"staging":{"host":"10.0.0.8","user":"deploy",
 * "port":22,"identity_file":"C:/keys/staging","proxy_jump":"jump"}}} */
static cJSON *ssh_profile_load(const tool_ctx *ctx, const char *name, ssh_profile *out) {
    char path[1024];
    char *raw;
    cJSON *root, *envs, *p;
    cJSON *v;
    if (!ctx || !ctx->state_root || !*ctx->state_root || !ssh_name_valid(name) || !out)
        return NULL;
    path_join(path, sizeof(path), ctx->state_root, "ssh/environments.json");
    raw = fs_read_file(path);
    if (!raw)
        return NULL;
    root = cJSON_Parse(raw);
    free(raw);
    envs = root ? cJSON_GetObjectItemCaseSensitive(root, "environments") : NULL;
    p = envs ? cJSON_GetObjectItemCaseSensitive(envs, name) : NULL;
    if (!p || !cJSON_IsObject(p)) {
        cJSON_Delete(root);
        return NULL;
    }
    memset(out, 0, sizeof(*out));
    v = cJSON_GetObjectItemCaseSensitive(p, "host");
    out->host = cJSON_IsString(v) ? v->valuestring : NULL;
    v = cJSON_GetObjectItemCaseSensitive(p, "user");
    out->user = cJSON_IsString(v) ? v->valuestring : NULL;
    v = cJSON_GetObjectItemCaseSensitive(p, "identity_file");
    out->identity_file = cJSON_IsString(v) ? v->valuestring : NULL;
    v = cJSON_GetObjectItemCaseSensitive(p, "proxy_jump");
    out->proxy_jump = cJSON_IsString(v) ? v->valuestring : NULL;
    v = cJSON_GetObjectItemCaseSensitive(p, "known_hosts");
    out->known_hosts = cJSON_IsString(v) ? v->valuestring : NULL;
    v = cJSON_GetObjectItemCaseSensitive(p, "port");
    out->port = cJSON_IsNumber(v) ? (int)v->valuedouble : 22;
    /* Values point into root. Keep it alive by transferring ownership through
     * a small leak-free convention: caller copies them before deleting root. */
    return root;
}

/* Extract a password only from the locally retained task text.  The planner
 * never receives this text verbatim (llm.c redacts secrets before egress), so
 * an ssh action can omit password while the built-in tool still authenticates. */
static char *ssh_password_from_task(const char *task) {
    const char *keys[] = {"password", "Password", "密码"};
    if (!task) return NULL;
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const char *p = strstr(task, keys[k]);
        if (!p) continue;
        p += strlen(keys[k]);
        while (*p == ' ' || *p == '\t' || *p == ':' || *p == '=' ||
               ((unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbc &&
                (unsigned char)p[2] == 0x9a)) { /* UTF-8 full-width colon */
            if ((unsigned char)p[0] == 0xef) p += 3;
            else p++;
        }
        const char *e = p;
        while (*e && !isspace((unsigned char)*e) && *e != ',' &&
               !((unsigned char)e[0] == 0xef && (unsigned char)e[1] == 0xbc &&
                 (unsigned char)e[2] == 0x8c)) e++; /* UTF-8 full-width comma */
        if (e > p) {
            size_t n = (size_t)(e - p);
            char *out = malloc(n + 1);
            if (out) { memcpy(out, p, n); out[n] = '\0'; }
            return out;
        }
    }
    return NULL;
}

static void ssh_secret_free(char *secret) {
    if (secret) {
        memset(secret, 0, strlen(secret));
        free(secret);
    }
}

static int ssh_absolute_dir(const char *dir, char *out, size_t cap) {
#if defined(_WIN32)
    wchar_t wide[2048], absolute[2048];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dir, -1, wide, 2048);
    if (n <= 0) return -1;
    DWORD len = GetFullPathNameW(wide, 2048, absolute, NULL);
    if (!len || len >= 2048) return -1;
    n = WideCharToMultiByte(CP_UTF8, 0, absolute, -1, out, (int)cap, NULL, NULL);
    return n > 0 ? 0 : -1;
#else
    if (dir[0] == '/')
        return snprintf(out, cap, "%s", dir) < (int)cap ? 0 : -1;
    char cwd[2048];
    if (!getcwd(cwd, sizeof(cwd))) return -1;
    return snprintf(out, cap, "%s/%s", cwd, dir) < (int)cap ? 0 : -1;
#endif
}

static int ssh_append_options(strbuf *out, const ssh_profile *profile, int has_profile,
                              const char *host, const char *user, const char *quoted_command,
                              int port, int batch_mode) {
    char *q;
    strbuf_appendf(out, "ssh -o BatchMode=%s -o ConnectTimeout=5 -o StrictHostKeyChecking=yes -p %d",
                   batch_mode ? "yes" : "no", port);
    if (has_profile && profile->identity_file) {
        q = ssh_quote_arg(profile->identity_file); if (!q) return -1;
        strbuf_appendf(out, " -i %s", q); free(q);
    }
    if (has_profile && profile->known_hosts) {
        q = ssh_quote_arg(profile->known_hosts); if (!q) return -1;
        strbuf_appendf(out, " -o UserKnownHostsFile=%s", q); free(q);
    }
    if (has_profile && profile->proxy_jump) {
        q = ssh_quote_arg(profile->proxy_jump); if (!q) return -1;
        strbuf_appendf(out, " -J %s", q); free(q);
    }
    if (user) strbuf_appendf(out, " -- %s@%s", user, host);
    else strbuf_appendf(out, " -- %s", host);
    strbuf_appendf(out, " %s", quoted_command);
    return 0;
}

/* OpenSSH's documented SSH_ASKPASS mechanism lets the built-in tool supply a
 * password without sshpass, Python or Paramiko.  The one-shot helper and its
 * secret file stay local under state/ssh and are removed after the command;
 * neither the password nor the original task text is included in the plan. */
static int ssh_append_askpass(strbuf *out, const tool_ctx *ctx, const ssh_profile *profile,
                              int has_profile, const char *host, const char *user,
                              const char *password, const char *quoted_command,
                              int port, char *helper, size_t helper_cap,
                              char *secret, size_t secret_cap) {
    char state_dir[1024], dir[2048], helper_name[96], secret_name[96];
    char *qhelper;
    strbuf data;
    int64_t stamp;
    unsigned pid;
    static atomic_uint next_id = 1;
    unsigned id = atomic_fetch_add(&next_id, 1);
    if (!ctx || !ctx->state_root || !*ctx->state_root || !password) return -1;
    path_join(state_dir, sizeof(state_dir), ctx->state_root, "ssh");
    if (ssh_absolute_dir(state_dir, dir, sizeof(dir)) != 0) return -1;
    if (fs_mkdirs(dir) != 0) return -1;
    stamp = time_now_ms();
#if defined(_WIN32)
    pid = (unsigned)GetCurrentProcessId();
    snprintf(helper_name, sizeof(helper_name), "askpass-%u-%lld-%u.cmd", pid, (long long)stamp, id);
    snprintf(secret_name, sizeof(secret_name), "askpass-%u-%lld-%u.secret", pid, (long long)stamp, id);
#else
    pid = (unsigned)getpid();
    snprintf(helper_name, sizeof(helper_name), "askpass-%u-%lld-%u.sh", pid, (long long)stamp, id);
    snprintf(secret_name, sizeof(secret_name), "askpass-%u-%lld-%u.secret", pid, (long long)stamp, id);
#endif
    if (strlen(dir) + strlen(helper_name) + 2 > helper_cap ||
        strlen(dir) + strlen(secret_name) + 2 > secret_cap) return -1;
    path_join(helper, helper_cap, dir, helper_name);
    path_join(secret, secret_cap, dir, secret_name);
    strbuf_init(&data);
#if defined(_WIN32)
    strbuf_append(&data, "@echo off\r\nsetlocal DisableDelayedExpansion\r\nset /p _COA_SSH_PASS=<\"%~dp0");
    strbuf_append(&data, secret_name);
    strbuf_append(&data, "\"\r\n<nul set /p \"=%_COA_SSH_PASS%\"\r\n");
#else
    strbuf_append(&data, "#!/bin/sh\ncat \"$(dirname \"$0\")/");
    strbuf_append(&data, secret_name);
    strbuf_append(&data, "\"\n");
#endif
    if (fs_write_file(helper, data.buf, data.len) != 0 ||
        fs_write_file(secret, password, strlen(password)) != 0) {
        strbuf_free(&data); fs_remove(helper); fs_remove(secret); return -1;
    }
    strbuf_free(&data);
    qhelper = ssh_quote_arg(helper);
    if (!qhelper) { fs_remove(helper); fs_remove(secret); return -1; }
#if defined(_WIN32)
    /* set \"name=value\" protects spaces in state_root; do not put the
     * shell-quoted form inside the value because OpenSSH expects a raw path. */
    strbuf_appendf(out, "set \"SSH_ASKPASS=%s\" & set \"SSH_ASKPASS_REQUIRE=force\" & set \"DISPLAY=1\" & ", helper);
#else
    if (chmod(helper, 0700) != 0 || chmod(secret, 0600) != 0) {
        free(qhelper); fs_remove(helper); fs_remove(secret); return -1;
    }
    strbuf_appendf(out, "SSH_ASKPASS=%s SSH_ASKPASS_REQUIRE=force DISPLAY=1 ", qhelper);
#endif
    if (ssh_append_options(out, profile, has_profile, host, user, quoted_command, port, 0) != 0) {
        free(qhelper); fs_remove(helper); fs_remove(secret); return -1;
    }
    free(qhelper);
    return 0;
}

static tool_result *ssh_exec(const tool *self, const tool_ctx *ctx, const char *args_json) {
    cJSON *args = cJSON_Parse(args_json);
    cJSON *host = args ? cJSON_GetObjectItemCaseSensitive(args, "host") : NULL;
    cJSON *environment = args ? cJSON_GetObjectItemCaseSensitive(args, "environment") : NULL;
    cJSON *user = args ? cJSON_GetObjectItemCaseSensitive(args, "user") : NULL;
    cJSON *command = args ? cJSON_GetObjectItemCaseSensitive(args, "command") : NULL;
    cJSON *port = args ? cJSON_GetObjectItemCaseSensitive(args, "port") : NULL;
    cJSON *timeout = args ? cJSON_GetObjectItemCaseSensitive(args, "timeout_ms") : NULL;
    cJSON *password = args ? cJSON_GetObjectItemCaseSensitive(args, "password") : NULL;
    int timeout_ms = timeout && cJSON_IsNumber(timeout) ? (int)timeout->valuedouble : 15000;
    int port_no = port && cJSON_IsNumber(port) ? (int)port->valuedouble : 22;
    ssh_profile profile;
    cJSON *profile_root = NULL;
    const char *host_text = host && cJSON_IsString(host) ? host->valuestring : NULL;
    const char *user_text = user && cJSON_IsString(user) ? user->valuestring : NULL;
    const char *password_text = password && cJSON_IsString(password) ? password->valuestring : NULL;
    char *task_password = NULL;
    char *quoted = NULL;
    char *shell_args = NULL;
    char askpass_helper[2300] = {0}, askpass_secret[2300] = {0};
    tool_result *result;
    strbuf cmd;
    (void)self;

    if (environment && cJSON_IsString(environment)) {
        profile_root = ssh_profile_load(ctx, environment->valuestring, &profile);
        if (!profile_root) {
            cJSON_Delete(args);
            return tool_result_new(0, "ssh: environment profile not found or invalid");
        }
        host_text = profile.host;
        user_text = profile.user;
        port_no = profile.port;
    }
    /* A planner only sees [REDACTED:secret]; resolve that marker from the
     * locally retained task instead of ever treating it as a credential. */
    if ((!password_text || strstr(password_text, "[REDACTED:secret]")) && ctx)
        password_text = task_password = ssh_password_from_task(ctx->task_input);
    if (!host_text || !ssh_host_valid(host_text) ||
        (user_text && !ssh_name_valid(user_text)) ||
        (password_text && (strchr(password_text, '\n') || strchr(password_text, '\r'))) ||
        !command || !cJSON_IsString(command) || port_no < 1 || port_no > 65535 ||
        (profile_root && ((profile.identity_file && !ssh_path_valid(profile.identity_file)) ||
                          (profile.known_hosts && !ssh_path_valid(profile.known_hosts)) ||
                          (profile.proxy_jump && !ssh_host_valid(profile.proxy_jump))))) {
        ssh_secret_free(task_password); cJSON_Delete(profile_root);
        cJSON_Delete(args);
        return tool_result_new(0, "ssh: host, command or port is invalid");
    }
    quoted = ssh_quote_arg(command->valuestring);
    if (!quoted) {
        ssh_secret_free(task_password); cJSON_Delete(profile_root); cJSON_Delete(args);
        return tool_result_new(0, "ssh: command must be a single line");
    }
    if (timeout_ms < 100)
        timeout_ms = 100;
    if (timeout_ms > 60000)
        timeout_ms = 60000;
    strbuf_init(&cmd);
    if (password_text) {
        if (ssh_append_askpass(&cmd, ctx, &profile, profile_root != NULL, host_text, user_text,
                               password_text, quoted, port_no, askpass_helper, sizeof(askpass_helper),
                               askpass_secret, sizeof(askpass_secret)) != 0) {
            ssh_secret_free(task_password); cJSON_Delete(profile_root); cJSON_Delete(args); free(quoted); strbuf_free(&cmd);
            return tool_result_new(0, "ssh: password arguments are invalid");
        }
    } else {
        if (ssh_append_options(&cmd, &profile, profile_root != NULL, host_text, user_text,
                               quoted, port_no, 1) != 0) {
            ssh_secret_free(task_password); cJSON_Delete(profile_root); cJSON_Delete(args); free(quoted); strbuf_free(&cmd);
            return tool_result_new(0, "ssh: profile arguments are invalid");
        }
    }
    cJSON *wrapped = cJSON_CreateObject();
    cJSON_AddStringToObject(wrapped, "command", cmd.buf);
    cJSON_AddNumberToObject(wrapped, "timeout_ms", timeout_ms);
    shell_args = cJSON_PrintUnformatted(wrapped);
    cJSON_Delete(wrapped);
    strbuf_free(&cmd);
    free(quoted);
    ssh_secret_free(task_password);
    cJSON_Delete(profile_root);
    cJSON_Delete(args);
    if (!shell_args) {
        if (askpass_helper[0]) fs_remove(askpass_helper);
        if (askpass_secret[0]) fs_remove(askpass_secret);
        return tool_result_new(0, "ssh: out of memory");
    }
    result = shell_exec_impl(NULL, ctx, shell_args, 1);
    if (askpass_helper[0]) fs_remove(askpass_helper);
    if (askpass_secret[0]) fs_remove(askpass_secret);
    free(shell_args);
    return result;
}

const tool *tool_ssh(void) {
    static const tool t = {
        "ssh",
        "Run one remote SSH command. Always use this tool for SSH work instead of shell. Supports named environments, keys/agent, and password authentication through the local system OpenSSH client without sshpass or Python.",
        "{\"type\":\"object\",\"properties\":{\"host\":{\"type\":\"string\"},\"environment\":{\"type\":\"string\"},\"user\":{\"type\":\"string\"},"
        "\"command\":{\"type\":\"string\"},\"password\":{\"type\":\"string\"},\"port\":{\"type\":\"integer\"},"
        "\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"command\"]}",
        1,
        ssh_exec,
        NULL,
    };
    return &t;
}
