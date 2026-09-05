/* wasm_runner.c — wasm3-backed Wasm runner for the plugin sandbox.
 * Parses a wasm module, loads it into an isolated wasm3 runtime, resolves the
 * target function, marshals numeric JSON args, calls it and returns the first
 * result as JSON. This is the "Wasm runner" the sandbox seam was designed for
 * (see sandbox.h); until this is registered, Wasm execution reports
 * "unsupported". */
#include "cognitive-os-agent/plugin_runtime/wasm_runner.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

#include "wasm3.h"

#define MAX_ARGS 16

int coa_wasm3_available(void) { return 1; }

char *coa_wasm3_run(const void *wasm, size_t wasm_len,
                   const char *fn_name, const char *args_json) {
    if (!wasm || !fn_name) return coa_strdup("{\"ok\":false,\"error\":\"bad arguments\"}");

    IM3Environment env = m3_NewEnvironment();
    if (!env) return coa_strdup("{\"ok\":false,\"error\":\"m3 environment failed\"}");
    IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!rt) { m3_FreeEnvironment(env); return coa_strdup("{\"ok\":false,\"error\":\"m3 runtime failed\"}"); }

    IM3Module mod = NULL;
    int loaded = 0;
    M3Result res = m3_ParseModule(env, &mod, wasm, wasm_len);
    if (!res) res = m3_LoadModule(rt, mod);
    if (!res) loaded = 1;

    IM3Function f = NULL;
    if (!res) res = m3_FindFunction(&f, rt, fn_name);

    const char *argv[MAX_ARGS];
    char buf[MAX_ARGS][32];
    int argc = 0;

    if (!res) {
        cJSON *root = args_json ? cJSON_Parse(args_json) : NULL;
        if (root) {
            cJSON *arr = cJSON_IsArray(root) ? root
                      : cJSON_GetObjectItemCaseSensitive(root, "args");
            if (arr && cJSON_IsArray(arr)) {
                int n = cJSON_GetArraySize(arr);
                if (n > MAX_ARGS) n = MAX_ARGS;
                for (int i = 0; i < n && argc < MAX_ARGS; i++) {
                    cJSON *v = cJSON_GetArrayItem(arr, i);
                    if (v && cJSON_IsNumber(v)) {
                        snprintf(buf[argc], 32, "%lld", (long long)v->valuedouble);
                        argv[argc] = buf[argc];
                        argc++;
                    }
                }
            } else {
                /* object of named numeric args, taken in key order */
                cJSON *it;
                cJSON_ArrayForEach(it, root) {
                    if (cJSON_IsNumber(it) && argc < MAX_ARGS) {
                        snprintf(buf[argc], 32, "%lld", (long long)it->valuedouble);
                        argv[argc] = buf[argc];
                        argc++;
                    }
                }
            }
        }
        if (root) cJSON_Delete(root);

        uint32_t want = m3_GetArgCount(f);
        if ((uint32_t)argc > want) argc = (int)want;
        res = m3_CallArgv(f, (uint32_t)argc, argv);
    }

    uint64_t ret = 0;
    if (!res) {
        const void *rets[1] = { &ret };
        res = m3_GetResults(f, 1, rets);
    }

    char out[640];
    if (res) {
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"wasm3: %s\"}",
                 res ? res : "?");
    } else {
        snprintf(out, sizeof(out), "{\"ok\":true,\"result\":%lld}",
                 (long long)(uint64_t)ret);
    }

    if (mod && !loaded) m3_FreeModule(mod);
    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    return coa_strdup(out);
}
