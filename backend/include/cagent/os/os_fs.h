/* os_fs.h — cross-platform filesystem helpers */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read an entire file into a NUL-terminated malloc'd string. NULL on error. */
char *ca_fs_read_file(const char *path);

/* Write bytes to a file (creates/truncates). 0 on success, -1 on error. */
int ca_fs_write_file(const char *path, const void *data, size_t len);

/* Append bytes to a file. 0 on success, -1 on error. */
int ca_fs_append_file(const char *path, const void *data, size_t len);

int ca_fs_exists(const char *path);
int ca_fs_is_dir(const char *path);
/* Recursively create a directory path. 0 ok, -1 on error. */
int ca_fs_mkdirs(const char *path);
/* Remove a file. 0 ok, -1 if missing/error. */
int ca_fs_remove(const char *path);

typedef struct ca_dir_entry {
    char *name;   /* base name only */
    int is_dir;
} ca_dir_entry;

typedef struct ca_dir_list {
    ca_dir_entry *items;
    size_t count;
    size_t cap;
} ca_dir_list;

/* List a directory. Returns 0 ok, -1 on error. Caller frees with ca_fs_list_free. */
int ca_fs_list_dir(const char *path, ca_dir_list *out);
void ca_fs_list_free(ca_dir_list *l);

#ifdef __cplusplus
}
#endif
