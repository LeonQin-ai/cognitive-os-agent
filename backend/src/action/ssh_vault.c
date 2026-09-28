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
    const char *last = ssh_field(root, "last_key");
    char *out = NULL;
    cJSON_ArrayForEach(p, arr) {
        const char *name = ssh_field(p, "environment"), *key = ssh_field(p, "key");
        if ((environment && name && strcmp(name, environment) == 0) ||
            (!environment && last && key && strcmp(key, last) == 0)) {
            out = cJSON_PrintUnformatted(p);
            break;
        }
    }
    cJSON_Delete(root);
    return out;
}

int ssh_session_profile_record(const char *state_root, const char *session_id,
                               const char *environment, const char *host, const char *user,
                               int port, const char *identity, const char *jump, const char *known) {
    char key[96];
    if (ssh_session_vault_key(session_id, host, user, port, key, sizeof(key))) return -1;
    cJSON *root = ssh_session_read(state_root, session_id);
    if (!root) return -1;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
        cJSON *p = cJSON_GetArrayItem(arr, i);
        const char *old = ssh_field(p, "key");
        if (old && strcmp(old, key) == 0) cJSON_DeleteItemFromArray(arr, i);
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "key", key);
    cJSON_AddStringToObject(p, "host", host);
    if (user && *user) cJSON_AddStringToObject(p, "user", user);
    cJSON_AddNumberToObject(p, "port", port);
    if (environment && *environment) cJSON_AddStringToObject(p, "environment", environment);
    if (identity && *identity) cJSON_AddStringToObject(p, "identity_file", identity);
    if (jump && *jump) cJSON_AddStringToObject(p, "proxy_jump", jump);
    if (known && *known) cJSON_AddStringToObject(p, "known_hosts", known);
    cJSON_AddItemToArray(arr, p);
    cJSON_DeleteItemFromObjectCaseSensitive(root, "last_key");
    cJSON_AddStringToObject(root, "last_key", key);
    int rc = ssh_session_write(state_root, session_id, root);
    cJSON_Delete(root);
    return rc;
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
        cJSON_DeleteItemFromObjectCaseSensitive(root, "last_key");
        int n = cJSON_GetArraySize(arr);
        if (n) {
            const char *last = ssh_field(cJSON_GetArrayItem(arr, n - 1), "key");
            if (last) cJSON_AddStringToObject(root, "last_key", last);
        }
    }
    int rc = found ? ssh_session_write(state_root, session_id, root) : -1;
    cJSON_Delete(root);
    if (!rc) ssh_vault_delete(state_root, key);
    return rc;
}

void ssh_session_profiles_clear(const char *state_root, const char *session_id) {
    cJSON *root = ssh_session_read(state_root, session_id);
    if (!root) return;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "profiles"), *p;
    cJSON_ArrayForEach(p, arr) {
        const char *key = ssh_field(p, "key");
        if (key) ssh_vault_delete(state_root, key);
    }
    cJSON_Delete(root);
    char dir[1024], path[1152];
    if (!ssh_session_path(state_root, session_id, dir, sizeof(dir), path, sizeof(path))) fs_remove(path);
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
