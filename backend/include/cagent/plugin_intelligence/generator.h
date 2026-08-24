/* generator.h — AI plugin generation (the ⭐ self-evolution loop).
 *
 * Pipeline: Requirement Analyzer -> Architect -> Code Generator ->
 * Security Review -> Registry + Skill registration.
 *
 * When a live LLM is configured (provider != "mock") the pipeline is driven by
 * the model: the planner-style prompt asks it to emit a JSON design with a
 * POSIX shell implementation. With the mock provider a deterministic template
 * is used so the whole loop is exercised offline (unit tests, CI).
 *
 * The generated script is (a) audited against the security rules and the
 * sandbox forbidden-command list, (b) registered as a versioned plugin in the
 * plugin registry with a content signature, (c) registered as a runnable
 * skill, and (d) persisted under <state_root>/plugins/<name>.sh. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cagent_ctx cagent_ctx;

/* Generate a plugin from a natural-language capability description.
 * Returns a malloc'd JSON object (caller frees):
 *   success: {"ok":true,"plugin":{"name","version","description","caps":[...],
 *             "signature"},"script":"...","skill":true,"path":"..."}
 *   failure: {"ok":false,"error":"..."}
 * Never returns NULL. */
char *ca_plugin_generate(cagent_ctx *ctx, const char *description);

#ifdef __cplusplus
}
#endif
