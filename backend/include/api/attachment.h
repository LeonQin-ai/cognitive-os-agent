#pragma once
#include <stddef.h>
#include "cJSON.h"
typedef struct memory memory;
#define ATTACHMENT_MAX_BYTES (64 * 1024 * 1024)
cJSON *attachment_store(const char *root, memory *m, const char *name,
                        const char *session, const char *body, size_t len);
cJSON *attachment_list(const char *root);
cJSON *attachment_image_preview(const char *root, const char *session, const char *id);
int attachment_delete(const char *root, memory *m, const char *id_or_name);
/* Immutable task snapshot. NULL on unknown, wrong-session or unusable input. */
char *attachment_snapshot(const char *root, const char *session, const cJSON *ids);
void attachment_reindex(const char *root, memory *m);
