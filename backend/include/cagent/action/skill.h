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

typedef struct ca_skill {
    const char *name;
    const char *description;
    const char *kind;      /* "shell" or "python" */
    const char *body;      /* shell command, or python source for kind=python */
} ca_skill;

typedef struct ca_skill_registry ca_skill_registry;

typedef struct ca_skill_result {
    int ok;            /* 1 = exit code 0 and not timed out */
    char *output;      /* combined stdout+stderr (malloc'd) */
} ca_skill_result;

ca_skill_registry *ca_skill_registry_new(void);
void ca_skill_registry_free(ca_skill_registry *r);

/* Register a skill (copies name/desc/kind/body). 0 ok, -1 duplicate/empty. */
int ca_skill_register(ca_skill_registry *r, const ca_skill *s);
const ca_skill *ca_skill_find(ca_skill_registry *r, const char *name);
int ca_skill_count(ca_skill_registry *r);
const ca_skill *ca_skill_get(ca_skill_registry *r, size_t i);

/* Execute a skill through the sandbox with the workspace as the working
 * directory. Returns a malloc'd result (never NULL on lookup success; NULL if
 * the skill is unknown or its command is forbidden). Caller frees with
 * ca_skill_result_free. args_json is reserved for future parameter binding. */
ca_skill_result *ca_skill_execute(ca_skill_registry *r, const char *name,
                                  const char *args_json, const char *workspace,
                                  int timeout_ms);
void ca_skill_result_free(ca_skill_result *res);

/* JSON array of skills {name,description,kind} (malloc'd; caller frees). */
char *ca_skill_list_json(ca_skill_registry *r);

#ifdef __cplusplus
}
#endif
