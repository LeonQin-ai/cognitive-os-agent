/* config.h — layered configuration: defaults -> JSON file -> environment.
 * Keys use dot notation ("llm.provider"). Backed by a cJSON object. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct config config;

/* Create config with a set of built-in defaults (NULL = none). */
config *config_new(void);
/* Merge a defaults JSON object (deep copy) into the config. */
int config_apply_json(config *c, const char *json_text);
/* Load and merge a JSON config file. Returns 0 ok, -1 if file unreadable/invalid. */
int config_load_file(config *c, const char *path);
/* Merge environment variables with prefix (e.g. "COA_" -> LLM_PROVIDER = llm.provider,
 * case-insensitive mapping: A_B maps to a.b). */
void config_apply_env(config *c, const char *prefix);
void config_free(config *c);

/* Getters. Returns NULL / default if missing. */
const char *config_get_str(const config *c, const char *key, const char *def);
int64_t config_get_int(const config *c, const char *key, int64_t def);
int config_get_bool(const config *c, const char *key, int def);

/* Render the whole config as a JSON string; caller frees. */
char *config_to_json(const config *c);

/* Set a dotted-path string value (used when persisting runtime changes). */
void config_set_str(config *c, const char *key, const char *value);
/* Set a dotted-path numeric value (persisted as a JSON number so
 * config_get_int reads it back after restart). */
void config_set_int(config *c, const char *key, int64_t value);
/* Serialize the whole config to a JSON file (atomic write). Returns 0 ok, -1 error. */
int config_save_file(config *c, const char *path);

#ifdef __cplusplus
}
#endif
