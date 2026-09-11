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

typedef struct skill {
    const char *name;
    const char *description;
    const char *kind; /* "shell" | "python" | "prompt" (LLM template) */
    const char *body; /* shell command, python source, or prompt template
                       * with {{placeholder}} args for kind=prompt */
    const char *caps; /* granted capability tokens, csv (e.g. "fs.read,net");
                       * NULL = unrestricted legacy skill */
} skill;

typedef struct skill_registry skill_registry;

typedef struct skill_result {
    int ok;       /* 1 = exit code 0 and not timed out */
    char *output; /* combined stdout+stderr (malloc'd) */
} skill_result;

skill_registry *skill_registry_new(void);
void skill_registry_free(skill_registry *r);

/* Register a skill (copies name/desc/kind/body). 0 ok, -1 duplicate/empty. */
int skill_register(skill_registry *r, const skill *s);
/* Register with upsert semantics: when replace is 1 an existing skill with the
 * same name is overwritten (used by the market "install" flow so reinstall and
 * update always succeed). 0 ok, -1 empty name/invalid kind. */
int skill_register_ex(skill_registry *r, const skill *s, int replace);
const skill *skill_find(skill_registry *r, const char *name);
int skill_count(skill_registry *r);
const skill *skill_get(skill_registry *r, size_t i);

/* Execute a skill through the sandbox with the workspace as the working
 * directory. Returns a malloc'd result (never NULL on lookup success; NULL if
 * the skill is unknown or its command is forbidden). Caller frees with
 * skill_result_free. args_json is reserved for future parameter binding.
 * kind="prompt" skills need an LLM and are NOT executed here — the caller
 * gets ok=0 with a hint to run them via skill_render_prompt + LLM. */
skill_result *skill_execute(skill_registry *r, const char *name, const char *args_json,
                                    const char *workspace, int timeout_ms);
void skill_result_free(skill_result *res);

/* Render a kind="prompt" skill: substitute {{placeholders}} from args_json and
 * return the final prompt text (malloc'd; caller frees). NULL when the skill
 * is unknown or its kind is not "prompt". */
char *skill_render_prompt(skill_registry *r, const char *name, const char *args_json);

/* JSON array of skills {name,description,kind} (malloc'd; caller frees). */
char *skill_list_json(skill_registry *r);

/* Remove a registered skill by name. 0 ok, -1 not found. */
int skill_unregister(skill_registry *r, const char *name);
/* Persist all skills to <state_root>/skills.json and reload on startup.
 * Load skips duplicates (e.g. the built-in echo_hello seeded at init). */
int skill_registry_persist(skill_registry *r, const char *state_root);
int skill_registry_load(skill_registry *r, const char *state_root);

#ifdef __cplusplus
}
#endif
