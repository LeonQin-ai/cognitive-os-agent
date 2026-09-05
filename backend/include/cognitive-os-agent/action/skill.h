/* skill.h — static skill registry (Shell/Python).
 * A skill is a named, reusable, versionless procedure that the planner can
 * invoke directly instead of composing raw tool actions. Skills are registered
 * statically (built-in or loaded from config) and executed through the sandbox
 * so dangerous commands are rejected. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_skill {
    const char *name;
    const char *description;
    const char *kind;      /* "shell" or "python" */
    const char *body;      /* shell command, or python source for kind=python */
    const char *caps;      /* granted capability tokens, csv (e.g. "fs.read,net");
                            * NULL = unrestricted legacy skill */
} coa_skill;

typedef struct coa_skill_registry coa_skill_registry;

typedef struct coa_skill_result {
    int ok;            /* 1 = exit code 0 and not timed out */
    char *output;      /* combined stdout+stderr (malloc'd) */
} coa_skill_result;

coa_skill_registry *coa_skill_registry_new(void);
void coa_skill_registry_free(coa_skill_registry *r);

/* Register a skill (copies name/desc/kind/body). 0 ok, -1 duplicate/empty. */
int coa_skill_register(coa_skill_registry *r, const coa_skill *s);
/* Register with upsert semantics: when replace is 1 an existing skill with the
 * same name is overwritten (used by the market "install" flow so reinstall and
 * update always succeed). 0 ok, -1 empty name/invalid kind. */
int coa_skill_register_ex(coa_skill_registry *r, const coa_skill *s, int replace);
const coa_skill *coa_skill_find(coa_skill_registry *r, const char *name);
int coa_skill_count(coa_skill_registry *r);
const coa_skill *coa_skill_get(coa_skill_registry *r, size_t i);

/* Execute a skill through the sandbox with the workspace as the working
 * directory. Returns a malloc'd result (never NULL on lookup success; NULL if
 * the skill is unknown or its command is forbidden). Caller frees with
 * coa_skill_result_free. args_json is reserved for future parameter binding. */
coa_skill_result *coa_skill_execute(coa_skill_registry *r, const char *name,
                                  const char *args_json, const char *workspace,
                                  int timeout_ms);
void coa_skill_result_free(coa_skill_result *res);

/* JSON array of skills {name,description,kind} (malloc'd; caller frees). */
char *coa_skill_list_json(coa_skill_registry *r);

/* Remove a registered skill by name. 0 ok, -1 not found. */
int coa_skill_unregister(coa_skill_registry *r, const char *name);
/* Persist all skills to <state_root>/skills.json and reload on startup.
 * Load skips duplicates (e.g. the built-in echo_hello seeded at init). */
int coa_skill_registry_persist(coa_skill_registry *r, const char *state_root);
int coa_skill_registry_load(coa_skill_registry *r, const char *state_root);

#ifdef __cplusplus
}
#endif
