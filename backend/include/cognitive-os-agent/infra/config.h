/* config.h — layered configuration: defaults -> JSON file -> environment.
 * Keys use dot notation ("llm.provider"). Backed by a cJSON object. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_config coa_config;

/* Create config with a set of built-in defaults (NULL = none). */
coa_config *coa_config_new(void);
/* Merge a defaults JSON object (deep copy) into the config. */
int coa_config_apply_json(coa_config *c, const char *json_text);
/* Load and merge a JSON config file. Returns 0 ok, -1 if file unreadable/invalid. */
int coa_config_load_file(coa_config *c, const char *path);
/* Merge environment variables with prefix (e.g. "COA_" -> COA_LLM_PROVIDER = llm.provider,
 * case-insensitive mapping: COA_A_B maps to a.b). */
void coa_config_apply_env(coa_config *c, const char *prefix);
void coa_config_free(coa_config *c);

/* Getters. Returns NULL / default if missing. */
const char *coa_config_get_str(const coa_config *c, const char *key, const char *def);
int64_t     coa_config_get_int(const coa_config *c, const char *key, int64_t def);
double      coa_config_get_dbl(const coa_config *c, const char *key, double def);
int         coa_config_get_bool(const coa_config *c, const char *key, int def);
int         coa_config_has(const coa_config *c, const char *key);

/* Render the whole config as a JSON string; caller frees. */
char *coa_config_to_json(const coa_config *c);

/* Set a dotted-path string value (used when persisting runtime changes). */
void coa_config_set_str(coa_config *c, const char *key, const char *value);
/* Set a dotted-path numeric value (persisted as a JSON number so
 * coa_config_get_int reads it back after restart). */
void coa_config_set_int(coa_config *c, const char *key, int64_t value);
/* Serialize the whole config to a JSON file (atomic write). Returns 0 ok, -1 error. */
int coa_config_save_file(coa_config *c, const char *path);

#ifdef __cplusplus
}
#endif
