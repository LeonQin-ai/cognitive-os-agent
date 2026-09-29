/* ssh_vault.c — local credentials keyed by SSH environment name.
 * Passwords never enter profiles, tool schemas, or model-visible results. */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif
#include "action/tools.h"
#include "os/os_fs.h"
#include "infra/util.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <Security/Security.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

int ssh_vault_name_valid(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n > 80) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.') return 0;
    return 1;
}

int ssh_vault_host_key(const char *host, const char *user, int port, char *out, size_t cap) {
    char endpoint[1024];
    if (!host || !*host || port < 1 || port > 65535 || !out || cap < 22 ||
        snprintf(endpoint, sizeof(endpoint), "%s|%s|%d", host, user ? user : "", port) >= (int)sizeof(endpoint))
        return -1;
    return snprintf(out, cap, "host-%016llx",
                    (unsigned long long)hash64(endpoint, strlen(endpoint))) < (int)cap ? 0 : -1;
}

int ssh_session_vault_key(const char *session_id, const char *host, const char *user,
                          int port, char *out, size_t cap) {
    char endpoint[96];
    if (!session_id || !*session_id ||
        ssh_vault_host_key(host, user, port, endpoint, sizeof(endpoint))) return -1;
    return snprintf(out, cap, "s%016llx-%s",
                    (unsigned long long)hash64(session_id, strlen(session_id)), endpoint) < (int)cap ? 0 : -1;
}

int ssh_session_pending_key(const char *key, char *out, size_t cap) {
    return ssh_vault_name_valid(key) && out &&
           snprintf(out, cap, "pending-%s", key) < (int)cap ? 0 : -1;
}

static int ssh_session_path(const char *state_root, const char *session_id,
                            char *dir, size_t dcap, char *path, size_t pcap) {
    if (!state_root || !*state_root || !session_id || !*session_id) return -1;
    path_join(dir, dcap, state_root, "ssh/sessions");
    return snprintf(path, pcap, "%s/%016llx.json", dir,
                    (unsigned long long)hash64(session_id, strlen(session_id))) < (int)pcap ? 0 : -1;
}

static cJSON *ssh_session_read(const char *state_root, const char *session_id) {
    char dir[1024], path[1152];
    if (ssh_session_path(state_root, session_id, dir, sizeof(dir), path, sizeof(path))) return NULL;
    char *raw = fs_read_file(path);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); root = cJSON_CreateObject(); }
    if (!root) return NULL;
    if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "profiles"))) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "profiles");
        cJSON_AddArrayToObject(root, "profiles");
    }
    return root;
}

static int ssh_session_write(const char *state_root, const char *session_id, cJSON *root) {
    char dir[1024], path[1152];
    if (ssh_session_path(state_root, session_id, dir, sizeof(dir), path, sizeof(path))) return -1;
    char *raw = cJSON_PrintUnformatted(root);
    int rc = raw && fs_mkdirs(dir) == 0 ? fs_write_file(path, raw, strlen(raw)) : -1;
#if !defined(_WIN32)
    if (!rc && chmod(path, 0600)) rc = -1;
#endif
    free(raw);
    return rc;
}

static const char *ssh_field(cJSON *o, const char *name) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

char *ssh_session_profile_get(const char *state_root, const char *session_id,
                              const char *environment) {
    cJSON *root = ssh_session_read(state_root, session_id);
    if (!root) return NULL;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles"), *p;
    const char *pending = ssh_field(root, "pending_key");
    const char *last = ssh_field(root, "last_key");
    char *out = NULL;
    for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
        p = cJSON_GetArrayItem(arr, i);
        const char *name = ssh_field(p, "environment"), *key = ssh_field(p, "key");
        if ((environment && name && strcmp(name, environment) == 0) ||
            (!environment && key && ((pending && strcmp(key, pending) == 0) ||
                                    (!pending && last && strcmp(key, last) == 0) ||
                                    (!pending && !last && i == cJSON_GetArraySize(arr) - 1)))) {
            out = cJSON_PrintUnformatted(p);
            break;
        }
    }
    cJSON_Delete(root);
    return out;
}

int ssh_session_has_verified_profile(const char *state_root, const char *session_id) {
    cJSON *root = ssh_session_read(state_root, session_id);
    if (!root) return 0;
    const char *last = ssh_field(root, "last_key");
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    int found = 0;
    cJSON *p;
    cJSON_ArrayForEach(p, arr) {
        const char *key = ssh_field(p, "key");
        if (last && key && strcmp(last, key) == 0 &&
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "verified"))) {
            found = 1;
            break;
        }
    }
    cJSON_Delete(root);
    return found;
}

static int ssh_session_profile_record_ex(const char *state_root, const char *session_id,
                                         const char *environment, const char *host, const char *user,
                                         int port, const char *identity, const char *jump,
                                         const char *known, int pending) {
    char key[96];
    if (ssh_session_vault_key(session_id, host, user, port, key, sizeof(key))) return -1;
    cJSON *root = ssh_session_read(state_root, session_id);
    if (!root) return -1;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    const char *prior_pending = ssh_field(root, "pending_key");
    char *obsolete_pending = pending && prior_pending && strcmp(prior_pending, key) != 0
                                 ? xstrdup(prior_pending) : NULL;
    char *old_env = NULL, *old_identity = NULL, *old_jump = NULL, *old_known = NULL;
    int was_verified = 0;
    for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
        cJSON *p = cJSON_GetArrayItem(arr, i);
        const char *old = ssh_field(p, "key");
        if (old && strcmp(old, key) == 0) {
            was_verified = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "verified"));
            const char *v;
            v = ssh_field(p, "environment"); if (v) old_env = xstrdup(v);
            v = ssh_field(p, "identity_file"); if (v) old_identity = xstrdup(v);
            v = ssh_field(p, "proxy_jump"); if (v) old_jump = xstrdup(v);
            v = ssh_field(p, "known_hosts"); if (v) old_known = xstrdup(v);
            cJSON_DeleteItemFromArray(arr, i);
        } else if (pending && obsolete_pending && old &&
                   strcmp(old, obsolete_pending) == 0 &&
                   !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "verified"))) {
            cJSON_DeleteItemFromArray(arr, i);
        }
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "key", key);
    cJSON_AddStringToObject(p, "host", host);
    if (user && *user) cJSON_AddStringToObject(p, "user", user);
    cJSON_AddNumberToObject(p, "port", port);
    if (!pending || was_verified) cJSON_AddBoolToObject(p, "verified", 1);
    if (pending) cJSON_AddBoolToObject(p, "pending", 1);
    if ((environment && *environment) || old_env) cJSON_AddStringToObject(p, "environment", environment && *environment ? environment : old_env);
    if ((identity && *identity) || old_identity) cJSON_AddStringToObject(p, "identity_file", identity && *identity ? identity : old_identity);
    if ((jump && *jump) || old_jump) cJSON_AddStringToObject(p, "proxy_jump", jump && *jump ? jump : old_jump);
    if ((known && *known) || old_known) cJSON_AddStringToObject(p, "known_hosts", known && *known ? known : old_known);
    cJSON_AddItemToArray(arr, p);
    if (pending) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "pending_key");
        cJSON_AddStringToObject(root, "pending_key", key);
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "last_key");
        cJSON_AddStringToObject(root, "last_key", key);
        cJSON_DeleteItemFromObjectCaseSensitive(root, "pending_key");
    }
    int rc = ssh_session_write(state_root, session_id, root);
    if (!rc && obsolete_pending) {
        char old_secret[96];
        if (!ssh_session_pending_key(obsolete_pending, old_secret, sizeof(old_secret)))
            ssh_vault_delete(state_root, old_secret);
    }
    free(obsolete_pending);
    free(old_env); free(old_identity); free(old_jump); free(old_known);
    cJSON_Delete(root);
    return rc;
}

int ssh_session_profile_record(const char *state_root, const char *session_id,
                               const char *environment, const char *host, const char *user,
                               int port, const char *identity, const char *jump, const char *known) {
    return ssh_session_profile_record_ex(state_root, session_id, environment, host, user,
                                         port, identity, jump, known, 0);
}

int ssh_session_profile_stage(const char *state_root, const char *session_id,
                              const char *environment, const char *host, const char *user,
                              int port, const char *identity, const char *jump, const char *known) {
    return ssh_session_profile_record_ex(state_root, session_id, environment, host, user,
                                         port, identity, jump, known, 1);
}

void ssh_session_profile_discard_pending(const char *state_root, const char *session_id,
                                         const char *key) {
    cJSON *root = ssh_session_read(state_root, session_id);
    if (!root) return;
    const char *pending = ssh_field(root, "pending_key");
    if (pending && key && strcmp(pending, key) == 0) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles");
        for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
            cJSON *p = cJSON_GetArrayItem(arr, i);
            const char *saved = ssh_field(p, "key");
            if (saved && strcmp(saved, key) == 0) {
                if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "verified")))
                    cJSON_DeleteItemFromObjectCaseSensitive(p, "pending");
                else
                    cJSON_DeleteItemFromArray(arr, i);
            }
        }
        cJSON_DeleteItemFromObjectCaseSensitive(root, "pending_key");
        ssh_session_write(state_root, session_id, root);
    }
    cJSON_Delete(root);
}

char *ssh_session_profiles_json(const char *state_root, const char *session_id) {
    cJSON *root = ssh_session_read(state_root, session_id);
    if (!root) return xstrdup("[]");
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles"), *p;
    cJSON *out = cJSON_CreateArray();
    cJSON_ArrayForEach(p, arr) {
        cJSON *item = cJSON_Duplicate(p, 1);
        const char *key = ssh_field(p, "key");
        char *secret = key ? ssh_vault_get(state_root, key) : NULL;
        if (!secret && key) {
            char pending_key[96];
            if (!ssh_session_pending_key(key, pending_key, sizeof(pending_key)))
                secret = ssh_vault_get(state_root, pending_key);
        }
        cJSON_AddBoolToObject(item, "has_password", secret != NULL);
        ssh_vault_secret_free(secret);
        cJSON_AddItemToArray(out, item);
    }
    char *json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out); cJSON_Delete(root);
    return json;
}

int ssh_session_profile_forget(const char *state_root, const char *session_id, const char *key) {
    if (!ssh_vault_name_valid(key)) return -1;
    cJSON *root = ssh_session_read(state_root, session_id);
    if (!root) return -1;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    int found = 0;
    for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
        cJSON *p = cJSON_GetArrayItem(arr, i);
        const char *old = ssh_field(p, "key");
        if (old && strcmp(old, key) == 0) { cJSON_DeleteItemFromArray(arr, i); found = 1; }
    }
    if (found) {
        const char *pending = ssh_field(root, "pending_key");
        if (pending && strcmp(pending, key) == 0)
            cJSON_DeleteItemFromObjectCaseSensitive(root, "pending_key");
        const char *last_key = ssh_field(root, "last_key");
        if (last_key && strcmp(last_key, key) == 0) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "last_key");
            for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
                cJSON *candidate = cJSON_GetArrayItem(arr, i);
                const char *candidate_key = ssh_field(candidate, "key");
                if (candidate_key && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(candidate, "verified"))) {
                    cJSON_AddStringToObject(root, "last_key", candidate_key);
                    break;
                }
            }
        }
    }
    int rc = found ? ssh_session_write(state_root, session_id, root) : -1;
    cJSON_Delete(root);
    if (!rc) {
        char pending_key[96];
        ssh_vault_delete(state_root, key);
        if (!ssh_session_pending_key(key, pending_key, sizeof(pending_key)))
            ssh_vault_delete(state_root, pending_key);
    }
    return rc;
}

void ssh_session_profiles_clear(const char *state_root, const char *session_id) {
    cJSON *root = ssh_session_read(state_root, session_id);
    if (!root) return;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles"), *p;
    cJSON_ArrayForEach(p, arr) {
        const char *key = ssh_field(p, "key");
        if (key) {
            char pending_key[96];
            ssh_vault_delete(state_root, key);
            if (!ssh_session_pending_key(key, pending_key, sizeof(pending_key)))
                ssh_vault_delete(state_root, pending_key);
        }
    }
    cJSON_Delete(root);
    char dir[1024], path[1152];
    if (!ssh_session_path(state_root, session_id, dir, sizeof(dir), path, sizeof(path))) fs_remove(path);
}

/* The prompt ingress parser deliberately recognizes only explicit fields.
 * It runs before task journaling/history/LLM calls, so the original password
 * is never retained in those surfaces. It does not infer a password from
 * ordinary phrases such as "password login". */
static const char *ssh_find_ascii(const char *text, const char *word) {
    size_t n = strlen(word);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)word[i])) i++;
        if (i == n && (p == text || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')) &&
            (!isalnum((unsigned char)p[n]) && p[n] != '_')) return p;
    }
    return NULL;
}

static const char *ssh_field_value_one(const char *text, const char *label, int ascii,
                                        int allow_is) {
    if (!label) return NULL;
    const char *cursor = text;
    while (*cursor) {
        const char *found = ascii ? ssh_find_ascii(cursor, label) : strstr(cursor, label);
        if (!found) return NULL;
        const char *p = found + strlen(label);
        const char *after_label = p;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ':' || *p == '=') p++;
        else if ((unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbc &&
                 (unsigned char)p[2] == 0x9a) p += 3;
        else if (allow_is && strncmp(p, "是", strlen("是")) == 0) p += strlen("是");
        else if (allow_is && strncmp(p, "is ", 3) == 0) p += 3;
        else if (p > after_label && (!allow_is ||
                 (strncmp(p, "login", 5) != 0 && strncmp(p, "登录", strlen("登录")) != 0 &&
                  strncmp(p, "认证", strlen("认证")) != 0 && strncmp(p, "方式", strlen("方式")) != 0))) {
            /* Natural language such as "host 10.0.0.1" or "密码 abc". */
        }
        else { cursor = found + strlen(label); continue; }
        while (*p == ' ' || *p == '\t') p++;
        if (*p) return p;
        return NULL;
    }
    return NULL;
}

static const char *ssh_field_value(const char *text, const char *english, const char *chinese,
                                    int allow_is) {
    const char *p = ssh_field_value_one(text, english, 1, allow_is);
    const char *q = ssh_field_value_one(text, chinese, 0, allow_is);
    return !p || (q && q < p) ? q : p;
}

static char *ssh_field_token(const char *p, size_t max_len, const char **end_out) {
    const char *start, *end;
    char quote = 0;
    if (!p) return NULL;
    if (*p == '\'' || *p == '"') quote = *p++;
    start = p;
    if (quote) {
        end = strchr(p, quote);
        if (!end) return NULL;
    } else {
        while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ';' &&
               !(strncmp(p, "，", strlen("，")) == 0) &&
               !(strncmp(p, "。", strlen("。")) == 0)) p++;
        end = p;
    }
    size_t n = (size_t)(end - start);
    if (!n || n > max_len) return NULL;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, start, n); out[n] = 0;
    if (end_out) *end_out = end;
    return out;
}

static int ssh_endpoint_token_valid(const char *s, int user) {
    if (!s || !*s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!isalnum(*p) && !strchr(user ? "._-" : "._-:[]", *p)) return 0;
    return 1;
}

static char *ssh_replace_secret(const char *prompt, const char *secret,
                                 const char *span_start, const char *span_end) {
    const char *marker = "[LOCAL_SSH_PASSWORD]";
    size_t slen = strlen(secret), mlen = strlen(marker), plen = strlen(prompt);
    int all = slen >= 4;
    size_t matches = 0;
    if (all) {
        for (const char *p = prompt; (p = strstr(p, secret)) != NULL; p += slen) matches++;
    } else matches = 1;
    if (!matches || matches > 128 || plen > SIZE_MAX - matches * mlen) return NULL;
    char *out = malloc(plen + matches * mlen + 1);
    if (!out) return NULL;
    char *dst = out;
    const char *p = prompt;
    while (*p) {
        const char *next = all ? strstr(p, secret) : (p <= span_start ? span_start : NULL);
        if (!next) break;
        size_t n = (size_t)(next - p);
        memcpy(dst, p, n); dst += n;
        memcpy(dst, marker, mlen); dst += mlen;
        p = all ? next + slen : span_end;
    }
    strcpy(dst, p);
    return out;
}

char *ssh_session_prepare_prompt(const char *state_root, const char *session_id,
                                 const char *prompt) {
    if (!prompt) return NULL;
    const char *ssh_word = ssh_find_ascii(prompt, "ssh");
    int ssh_intent = ssh_word || strstr(prompt, "远程登录") || strstr(prompt, "登录服务器") ||
                     strstr(prompt, "连接服务器");
    /* After a failed login, a short follow-up often says only "用户名/密码是…".
     * Treat it as an SSH correction when this chat already has an endpoint. */
    if (!ssh_intent && state_root && *state_root &&
        (ssh_field_value(prompt, "password", "密码", 1) ||
         ssh_field_value(prompt, "username", "用户名", 0) ||
         ssh_field_value(prompt, "account", "账号", 0))) {
        char *saved = ssh_session_profile_get(state_root,
            session_id && *session_id ? session_id : "default", NULL);
        ssh_intent = saved != NULL;
        free(saved);
    }
    if (!ssh_intent) return xstrdup(prompt);
    const char *password_at = ssh_field_value(prompt, "password", "密码", 1);
    const char *password_end = NULL;
    char *password = ssh_field_token(password_at, 256, &password_end);
    if (password && (strncmp(password, "[REDACTED:", 10) == 0 ||
                     strcmp(password, "[LOCAL_SSH_PASSWORD]") == 0)) {
        ssh_vault_secret_free(password);
        password = NULL;
    }
    char *safe = password ? ssh_replace_secret(prompt, password, password_at, password_end)
                          : xstrdup(prompt);
    if (!safe) { ssh_vault_secret_free(password); return NULL; }

    if (!state_root || !*state_root) {
        if (password) { free(safe); safe = NULL; }
        ssh_vault_secret_free(password);
        return safe;
    }
    const char *sid = session_id && *session_id ? session_id : "default";
    char *host = NULL, *user = NULL, *environment = NULL;
    char *identity = NULL, *jump = NULL, *known = NULL;
    int port = 22;
    const char *p = ssh_field_value(prompt, "host", "主机", 0);
    if (!p) p = ssh_field_value(prompt, "server", "服务器", 0);
    if (!p) p = ssh_field_value(prompt, "ip", "地址", 0);
    host = ssh_field_token(p, 255, NULL);
    p = ssh_field_value(prompt, "user", "用户名", 0);
    if (!p) p = ssh_field_value(prompt, "username", "用户", 0);
    if (!p) p = ssh_field_value(prompt, "account", "账号", 0);
    user = ssh_field_token(p, 80, NULL);
    p = ssh_field_value(prompt, "environment", "环境", 0);
    environment = ssh_field_token(p, 80, NULL);
    p = ssh_field_value(prompt, "port", "端口", 0);
    if (!p && ssh_word) {
        const char *flag = strstr(ssh_word, "-p ");
        if (flag) p = flag + 3;
    }
    if (p) {
        char *end = NULL;
        long parsed = strtol(p, &end, 10);
        if (end != p && parsed >= 1 && parsed <= 65535) port = (int)parsed;
    }
    /* Accept the common ssh user@host spelling even without labelled fields. */
    if (!host) {
        for (const char *at = strchr(prompt, '@'); at; at = strchr(at + 1, '@')) {
            const char *begin = at;
            const char *end = at + 1;
            while (begin > prompt && (isalnum((unsigned char)begin[-1]) ||
                   strchr("._-", begin[-1]))) begin--;
            while (*end && (isalnum((unsigned char)*end) || strchr("._-", *end))) end++;
            if (begin == at || end == at + 1 || (size_t)(at - begin) > 80 ||
                (size_t)(end - at - 1) > 255) continue;
            char *candidate_user = malloc((size_t)(at - begin) + 1);
            char *candidate_host = malloc((size_t)(end - at));
            if (!candidate_user || !candidate_host) { free(candidate_user); free(candidate_host); break; }
            memcpy(candidate_user, begin, (size_t)(at - begin)); candidate_user[at - begin] = 0;
            memcpy(candidate_host, at + 1, (size_t)(end - at - 1)); candidate_host[end - at - 1] = 0;
            if (ssh_endpoint_token_valid(candidate_user, 1) &&
                ssh_endpoint_token_valid(candidate_host, 0)) {
                host = candidate_host;
                if (!user) user = candidate_user; else free(candidate_user);
                break;
            }
            free(candidate_user); free(candidate_host);
        }
    }
    if (host && !ssh_endpoint_token_valid(host, 0)) { free(host); host = NULL; }
    if (user && !ssh_endpoint_token_valid(user, 1)) { free(user); user = NULL; }
    if (environment && !ssh_vault_name_valid(environment)) { free(environment); environment = NULL; }
    if (!host && (password || user)) {
        char *raw = ssh_session_profile_get(state_root, sid, environment);
        cJSON *profile = raw ? cJSON_Parse(raw) : NULL;
        int use_saved = !environment ||
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(profile, "pending"));
        const char *saved_host = profile ? ssh_field(profile, "host") : NULL;
        const char *saved_user = profile ? ssh_field(profile, "user") : NULL;
        const char *saved_env = profile ? ssh_field(profile, "environment") : NULL;
        const char *saved_identity = profile ? ssh_field(profile, "identity_file") : NULL;
        const char *saved_jump = profile ? ssh_field(profile, "proxy_jump") : NULL;
        const char *saved_known = profile ? ssh_field(profile, "known_hosts") : NULL;
        cJSON *saved_port = profile ? cJSON_GetObjectItemCaseSensitive(profile, "port") : NULL;
        if (use_saved) {
            if (saved_host) host = xstrdup(saved_host);
            if (!user && saved_user) user = xstrdup(saved_user);
            if (!environment && saved_env) environment = xstrdup(saved_env);
            if (saved_identity) identity = xstrdup(saved_identity);
            if (saved_jump) jump = xstrdup(saved_jump);
            if (saved_known) known = xstrdup(saved_known);
            if (saved_port && cJSON_IsNumber(saved_port)) port = (int)saved_port->valuedouble;
        }
        cJSON_Delete(profile); free(raw);
    }
    if (!host && environment) {
        char path[1152];
        path_join(path, sizeof(path), state_root, "ssh/environments.json");
        char *raw = fs_read_file(path);
        cJSON *root = raw ? cJSON_Parse(raw) : NULL;
        cJSON *envs = root ? cJSON_GetObjectItemCaseSensitive(root, "environments") : NULL;
        cJSON *entry = envs ? cJSON_GetObjectItemCaseSensitive(envs, environment) : NULL;
        const char *saved_host = entry ? ssh_field(entry, "host") : NULL;
        const char *saved_user = entry ? ssh_field(entry, "user") : NULL;
        const char *saved_identity = entry ? ssh_field(entry, "identity_file") : NULL;
        const char *saved_jump = entry ? ssh_field(entry, "proxy_jump") : NULL;
        const char *saved_known = entry ? ssh_field(entry, "known_hosts") : NULL;
        cJSON *saved_port = entry ? cJSON_GetObjectItemCaseSensitive(entry, "port") : NULL;
        if (saved_host) host = xstrdup(saved_host);
        if (!user && saved_user) user = xstrdup(saved_user);
        if (saved_identity) identity = xstrdup(saved_identity);
        if (saved_jump) jump = xstrdup(saved_jump);
        if (saved_known) known = xstrdup(saved_known);
        if (saved_port && cJSON_IsNumber(saved_port)) port = (int)saved_port->valuedouble;
        cJSON_Delete(root); free(raw);
    }
    if (!host && (password || user)) {
        char *raw = ssh_session_profile_get(state_root, sid, environment);
        cJSON *profile = raw ? cJSON_Parse(raw) : NULL;
        const char *saved_host = profile ? ssh_field(profile, "host") : NULL;
        const char *saved_user = profile ? ssh_field(profile, "user") : NULL;
        const char *saved_identity = profile ? ssh_field(profile, "identity_file") : NULL;
        const char *saved_jump = profile ? ssh_field(profile, "proxy_jump") : NULL;
        const char *saved_known = profile ? ssh_field(profile, "known_hosts") : NULL;
        cJSON *saved_port = profile ? cJSON_GetObjectItemCaseSensitive(profile, "port") : NULL;
        if (saved_host) host = xstrdup(saved_host);
        if (!user && saved_user) user = xstrdup(saved_user);
        if (saved_identity) identity = xstrdup(saved_identity);
        if (saved_jump) jump = xstrdup(saved_jump);
        if (saved_known) known = xstrdup(saved_known);
        if (saved_port && cJSON_IsNumber(saved_port)) port = (int)saved_port->valuedouble;
        cJSON_Delete(profile); free(raw);
    }
    if (host && !ssh_endpoint_token_valid(host, 0)) { free(host); host = NULL; }
    if (user && !ssh_endpoint_token_valid(user, 1)) { free(user); user = NULL; }
    if (password && !host) { free(safe); safe = NULL; }
    if (host) {
        char key[96] = {0}, pending_key[96];
        if (ssh_session_vault_key(sid, host, user, port, key, sizeof(key)) ||
            ssh_session_pending_key(key, pending_key, sizeof(pending_key)) ||
            ssh_session_profile_stage(state_root, sid, environment,
                                      host, user, port, identity, jump, known) ||
            (password && ssh_vault_put(state_root, pending_key, password))) {
            if (*key) ssh_session_profile_discard_pending(state_root, sid, key);
            free(safe); safe = NULL;
        }
    }
    free(host); free(user); free(environment);
    free(identity); free(jump); free(known);
    ssh_vault_secret_free(password);
    return safe;
}

void ssh_vault_secret_free(char *secret) {
    if (!secret) return;
    volatile char *p = (volatile char *)secret;
    for (size_t n = strlen(secret); n; n--) *p++ = 0;
    free(secret);
}

#if defined(__APPLE__)
static int keychain_key(const char *state_root, const char *name, char *out, size_t cap) {
    return snprintf(out, cap, "%s:ssh:%s", state_root, name) < (int)cap ? 0 : -1;
}

int ssh_vault_put(const char *state_root, const char *name, const char *password) {
    char key[1024];
    SecKeychainItemRef item = NULL;
    UInt32 old_len = 0;
    void *old_data = NULL;
    OSStatus rc;
    if (!state_root || !*state_root || !ssh_vault_name_valid(name) || !password ||
        !*password || strlen(password) > 1024 || keychain_key(state_root, name, key, sizeof(key))) return -1;
    rc = SecKeychainFindGenericPassword(NULL, 18, "cognitive-os-agent", (UInt32)strlen(key), key,
                                         &old_len, &old_data, &item);
    if (old_data) SecKeychainItemFreeContent(NULL, old_data);
    if (rc == errSecSuccess) {
        rc = SecKeychainItemModifyContent(item, NULL, (UInt32)strlen(password), password);
        CFRelease(item);
    } else if (rc == errSecItemNotFound) {
        rc = SecKeychainAddGenericPassword(NULL, 18, "cognitive-os-agent", (UInt32)strlen(key), key,
                                            (UInt32)strlen(password), password, NULL);
    }
    return rc == errSecSuccess ? 0 : -1;
}

char *ssh_vault_get(const char *state_root, const char *name) {
    char key[1024], *out = NULL;
    UInt32 len = 0;
    void *data = NULL;
    if (!state_root || !ssh_vault_name_valid(name) || keychain_key(state_root, name, key, sizeof(key))) return NULL;
    if (SecKeychainFindGenericPassword(NULL, 18, "cognitive-os-agent", (UInt32)strlen(key), key,
                                       &len, &data, NULL) != errSecSuccess) return NULL;
    if (len > 0 && len <= 1024) {
        out = malloc((size_t)len + 1);
        if (out) { memcpy(out, data, len); out[len] = 0; }
    }
    SecKeychainItemFreeContent(NULL, data);
    return out;
}

int ssh_vault_delete(const char *state_root, const char *name) {
    char key[1024];
    SecKeychainItemRef item = NULL;
    if (!state_root || !ssh_vault_name_valid(name) || keychain_key(state_root, name, key, sizeof(key))) return -1;
    OSStatus rc = SecKeychainFindGenericPassword(NULL, 18, "cognitive-os-agent", (UInt32)strlen(key), key,
                                                 NULL, NULL, &item);
    if (rc == errSecItemNotFound) return 0;
    if (rc != errSecSuccess) return -1;
    rc = SecKeychainItemDelete(item);
    CFRelease(item);
    return rc == errSecSuccess ? 0 : -1;
}

#else
static int vault_path(const char *state_root, const char *name, char *dir, size_t dcap,
                      char *path, size_t pcap) {
    if (!state_root || !*state_root || !ssh_vault_name_valid(name)) return -1;
    path_join(dir, dcap, state_root, "ssh/vault");
    if (strlen(dir) + strlen(name) + 6 >= pcap) return -1;
    snprintf(path, pcap, "%s/%s.cred", dir, name);
    return 0;
}

#if defined(_WIN32)
static char hex_digit(unsigned v) { return "0123456789abcdef"[v & 15]; }
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
#endif

int ssh_vault_put(const char *state_root, const char *name, const char *password) {
    char dir[1024], path[1152];
    size_t len = password ? strlen(password) : 0;
    if (!len || len > 1024 || vault_path(state_root, name, dir, sizeof(dir), path, sizeof(path))) return -1;
    if (fs_mkdirs(dir)) return -1;
#if defined(_WIN32)
    DATA_BLOB input = {(DWORD)len, (BYTE *)password}, encrypted = {0};
    if (!CryptProtectData(&input, L"cognitive-os-agent SSH", NULL, NULL, NULL,
                          CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) return -1;
    char *hex = malloc((size_t)encrypted.cbData * 2 + 1);
    if (!hex) { LocalFree(encrypted.pbData); return -1; }
    for (DWORD i = 0; i < encrypted.cbData; i++) {
        hex[2 * i] = hex_digit(encrypted.pbData[i] >> 4);
        hex[2 * i + 1] = hex_digit(encrypted.pbData[i]);
    }
    hex[(size_t)encrypted.cbData * 2] = 0;
    int rc = fs_write_file(path, hex, (size_t)encrypted.cbData * 2);
    SecureZeroMemory(hex, (size_t)encrypted.cbData * 2); free(hex);
    LocalFree(encrypted.pbData);
    return rc;
#else
    /* Linux has no universal built-in user keychain API. Store credentials
     * with the same owner-only protection OpenSSH requires for private keys. */
    if (chmod(dir, 0700)) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int rc = (fchmod(fd, 0600) == 0 && write(fd, password, len) == (ssize_t)len) ? 0 : -1;
    if (close(fd)) rc = -1;
    if (rc) unlink(path);
    return rc;
#endif
}

char *ssh_vault_get(const char *state_root, const char *name) {
    char dir[1024], path[1152];
    if (vault_path(state_root, name, dir, sizeof(dir), path, sizeof(path))) return NULL;
#if defined(_WIN32)
    char *hex = fs_read_file(path);
    if (!hex) return NULL;
    size_t n = strlen(hex);
    if (!n || n % 2 || n > 8192) { free(hex); return NULL; }
    BYTE *bytes = malloc(n / 2);
    if (!bytes) { free(hex); return NULL; }
    for (size_t i = 0; i < n / 2; i++) {
        int a = hex_value(hex[2 * i]), b = hex_value(hex[2 * i + 1]);
        if (a < 0 || b < 0) { free(bytes); free(hex); return NULL; }
        bytes[i] = (BYTE)((a << 4) | b);
    }
    free(hex);
    DATA_BLOB input = {(DWORD)(n / 2), bytes}, plain = {0};
    BOOL ok = CryptUnprotectData(&input, NULL, NULL, NULL, NULL,
                                 CRYPTPROTECT_UI_FORBIDDEN, &plain);
    free(bytes);
    if (!ok) return NULL;
    char *out = NULL;
    if (plain.cbData && plain.cbData <= 1024) {
        out = malloc((size_t)plain.cbData + 1);
        if (out) { memcpy(out, plain.pbData, plain.cbData); out[plain.cbData] = 0; }
    }
    SecureZeroMemory(plain.pbData, plain.cbData); LocalFree(plain.pbData);
    return out;
#else
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 077) || st.st_size < 1 || st.st_size > 1024) {
        close(fd); return NULL;
    }
    char *out = malloc((size_t)st.st_size + 1);
    if (!out) { close(fd); return NULL; }
    ssize_t got = read(fd, out, (size_t)st.st_size);
    close(fd);
    if (got != st.st_size) { free(out); return NULL; }
    out[got] = 0;
    return out;
#endif
}

int ssh_vault_delete(const char *state_root, const char *name) {
    char dir[1024], path[1152];
    if (vault_path(state_root, name, dir, sizeof(dir), path, sizeof(path))) return -1;
    return !fs_exists(path) || fs_remove(path) == 0 ? 0 : -1;
}
#endif
