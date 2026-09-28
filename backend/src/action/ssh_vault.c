/* ssh_vault.c — local credentials keyed by SSH environment name.
 * Passwords never enter profiles, tool schemas, or model-visible results. */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif
#include "action/tools.h"
#include "os/os_fs.h"
#include "infra/util.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <Security/Security.h>
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
