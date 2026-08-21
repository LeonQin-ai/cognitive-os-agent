/* config.h — layered configuration: defaults -> JSON file -> environment.
 * Keys use dot notation ("llm.provider"). Backed by a cJSON object. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_config ca_config;

/* Create config with a set of built-in defaults (NULL = none). */
ca_config *ca_config_new(void);
/* Merge a defaults JSON object (deep copy) into the config. */
int ca_config_apply_json(ca_config *c, const char *json_text);
/* Load and merge a JSON config file. Returns 0 ok, -1 if file unreadable/invalid. */
int ca_config_load_file(ca_config *c, const char *path);
/* Merge environment variables with prefix (e.g. "CA_" -> CA_LLM_PROVIDER = llm.provider,
 * case-insensitive mapping: CA_A_B maps to a.b). */
void ca_config_apply_env(ca_config *c, const char *prefix);
void ca_config_free(ca_config *c);

/* Getters. Returns NULL / default if missing. */
const char *ca_config_get_str(const ca_config *c, const char *key, const char *def);
int64_t     ca_config_get_int(const ca_config *c, const char *key, int64_t def);
double      ca_config_get_dbl(const ca_config *c, const char *key, double def);
int         ca_config_get_bool(const ca_config *c, const char *key, int def);
int         ca_config_has(const ca_config *c, const char *key);

/* Render the whole config as a JSON string; caller frees. */
char *ca_config_to_json(const ca_config *c);

/* Set a dotted-path string value (used when persisting runtime changes). */
void ca_config_set_str(ca_config *c, const char *key, const char *value);
/* Serialize the whole config to a JSON file (atomic write). Returns 0 ok, -1 error. */
int ca_config_save_file(ca_config *c, const char *path);

#ifdef __cplusplus
}
#endif
