/* wasm_runner.c — numeric and UTF-8 plugin calls in an isolated wasm3 runtime. */
#include "plugin_runtime/wasm_runner.h"
#include "infra/util.h"
#include "cJSON.h"
#include "wasm3.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ARGS 16
#define MAX_TEXT (1024 * 1024)

static char *wasm_reply(const char *error, cJSON *value) {
    cJSON *root = cJSON_CreateObject();
    if (!root) { cJSON_Delete(value); return NULL; }
    cJSON_AddBoolToObject(root, "ok", error == NULL);
    if (error) {
        cJSON_AddStringToObject(root, "error", error);
        cJSON_Delete(value);
    } else if (value)
        cJSON_AddItemToObject(root, "result", value);
    else
        cJSON_AddNullToObject(root, "result");
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static M3Result marshal_number(IM3Function fn, int index, double number, char *out, size_t cap) {
    if (!isfinite(number)) return "non-finite numeric argument";
    M3ValueType type = m3_GetArgType(fn, (uint32_t)index);
    if (type == c_m3Type_i32 || type == c_m3Type_i64) {
        double limit = type == c_m3Type_i32 ? 2147483647.0 : 9007199254740991.0;
        double minimum = type == c_m3Type_i32 ? -2147483648.0 : -limit;
        if (number < minimum || number > limit || trunc(number) != number)
            return "integer argument is fractional or outside the exact JSON range";
        snprintf(out, cap, "%lld", (long long)number);
    } else if (type == c_m3Type_f32 || type == c_m3Type_f64) {
        if (type == c_m3Type_f32 && (number < -FLT_MAX || number > FLT_MAX))
            return "f32 argument out of range";
        snprintf(out, cap, "%.17g", number);
    } else return "unsupported argument type";
    return NULL;
}

/* The guest owns allocation. Always reacquire memory after calls: the guest
 * allocator may grow it. All buffers disappear with this per-call runtime. */
static M3Result marshal_text(IM3Runtime rt, const char *text, uint32_t *ptr, uint32_t *len) {
    size_t length = strlen(text);
    if (length > MAX_TEXT || !str_utf8_valid_n(text, (long long)length))
        return "text argument exceeds 1 MiB or is not UTF-8";
    IM3Function alloc = NULL;
    M3Result res = m3_FindFunction(&alloc, rt, "coa_alloc");
    if (res) return "text arguments require exported coa_alloc(i32)->i32";
    if (m3_GetArgCount(alloc) != 1 || m3_GetArgType(alloc, 0) != c_m3Type_i32 ||
        m3_GetRetCount(alloc) != 1 || m3_GetRetType(alloc, 0) != c_m3Type_i32)
        return "invalid coa_alloc signature";
    char size_arg[32];
    snprintf(size_arg, sizeof(size_arg), "%u", (unsigned)length + 1);
    const char *args[] = {size_arg};
    res = m3_CallArgv(alloc, 1, args);
    const void *rets[] = {ptr};
    if (!res) res = m3_GetResults(alloc, 1, rets);
    if (res) return res;
    uint32_t size = 0;
    uint8_t *memory = m3_GetMemory(rt, &size, 0);
    if (!memory || !*ptr || *ptr > size || length + 1 > size - *ptr)
        return "coa_alloc returned an invalid memory range";
    memcpy(memory + *ptr, text, length + 1);
    *len = (uint32_t)length;
    return NULL;
}

int wasm3_available(void) { return 1; }

char *wasm3_run(const void *wasm, size_t wasm_len, const char *fn_name, const char *args_json) {
    if (!wasm || !wasm_len || wasm_len > UINT32_MAX || !fn_name || !*fn_name)
        return wasm_reply("bad arguments", NULL);
    /* cJSON stores strings as NUL-terminated buffers. Reject embedded NUL
     * instead of silently dropping the remainder of a decoded argument. */
    if (args_json) {
        for (const char *p = args_json; *p; p++) {
            if (*p == '\\' && p[1]) {
                if (strncmp(p + 1, "u0000", 5) == 0)
                    return wasm_reply("embedded NUL is unsupported in text arguments", NULL);
                p++;
            }
        }
    }
    cJSON *root = cJSON_ParseWithOpts(args_json ? args_json : "[]", NULL, 1);
    if (!root || (!cJSON_IsArray(root) && !cJSON_IsObject(root))) {
        cJSON_Delete(root);
        return wasm_reply("arguments must be a JSON array or object", NULL);
    }
    cJSON *args = root;
    int text_result = 0;
    if (cJSON_IsObject(root)) {
        cJSON *envelope = cJSON_GetObjectItemCaseSensitive(root, "args");
        if (envelope) {
            cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "result");
            if (!cJSON_IsArray(envelope) ||
                (format && (!cJSON_IsString(format) || strcmp(format->valuestring, "utf8") != 0))) {
                cJSON_Delete(root);
                return wasm_reply("expected {args:[...],result:\"utf8\"} or {args:[...]}", NULL);
            }
            args = envelope;
            text_result = format != NULL;
        }
    }

    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = env ? m3_NewRuntime(env, 64 * 1024, NULL) : NULL;
    if (!rt) {
        if (env) m3_FreeEnvironment(env);
        cJSON_Delete(root);
        return wasm_reply("wasm3 runtime allocation failed", NULL);
    }
    IM3Module mod = NULL;
    IM3Function fn = NULL;
    int loaded = 0;
    cJSON *value = NULL;
    M3Result res = m3_ParseModule(env, &mod, wasm, (uint32_t)wasm_len);
    if (!res) res = m3_LoadModule(rt, mod);
    if (!res) loaded = 1;
    if (!res) res = m3_FindFunction(&fn, rt, fn_name);

    const char *argv[MAX_ARGS];
    char buffers[MAX_ARGS][64];
    int argc = 0;
    uint32_t want = !res ? m3_GetArgCount(fn) : 0;
    if (!res && want > MAX_ARGS) res = "too many function arguments (max 16)";
    if (!res && (m3_GetRetCount(fn) > 1 ||
        (text_result && (m3_GetRetCount(fn) != 1 || m3_GetRetType(fn, 0) != c_m3Type_i64))))
        res = "unsupported return signature; utf8 requires one packed i64";
    cJSON *arg;
    cJSON_ArrayForEach(arg, args) {
        if (res) break;
        if (cJSON_IsNumber(arg)) {
            if ((uint32_t)argc >= want) { res = "too many arguments"; break; }
            res = marshal_number(fn, argc, arg->valuedouble, buffers[argc], sizeof(buffers[argc]));
            argv[argc] = buffers[argc];
            argc++;
        } else if (cJSON_IsString(arg)) {
            if ((uint32_t)argc + 2 > want ||
                m3_GetArgType(fn, (uint32_t)argc) != c_m3Type_i32 ||
                m3_GetArgType(fn, (uint32_t)argc + 1) != c_m3Type_i32) {
                res = "text arguments require an i32 pointer/length pair";
                break;
            }
            uint32_t ptr = 0, len = 0;
            res = marshal_text(rt, arg->valuestring, &ptr, &len);
            if (res) break;
            snprintf(buffers[argc], sizeof(buffers[argc]), "%u", (unsigned)ptr);
            argv[argc] = buffers[argc];
            argc++;
            snprintf(buffers[argc], sizeof(buffers[argc]), "%u", (unsigned)len);
            argv[argc] = buffers[argc];
            argc++;
        } else res = "arguments must be numbers or UTF-8 strings";
    }
    if (!res && (uint32_t)argc != want) res = "argument count mismatch";
    if (!res) res = m3_CallArgv(fn, (uint32_t)argc, argv);
    if (!res && m3_GetRetCount(fn)) {
        union { int32_t i32; int64_t i64; float f32; double f64; } result = {0};
        const void *rets[] = {&result};
        res = m3_GetResults(fn, 1, rets);
        if (!res && text_result) {
            uint64_t packed = (uint64_t)result.i64;
            uint32_t ptr = (uint32_t)packed, len = (uint32_t)(packed >> 32), size = 0;
            uint8_t *memory = m3_GetMemory(rt, &size, 0);
            if (!memory || len > MAX_TEXT || ptr > size || len > size - ptr)
                res = "text result outside guest memory or exceeds 1 MiB";
            else if (memchr(memory + ptr, 0, len) || !str_utf8_valid_n((const char *)memory + ptr, len))
                res = "text result must be UTF-8 without embedded NUL";
            else {
                char *text = malloc((size_t)len + 1);
                if (!text) res = "text result allocation failed";
                else {
                    memcpy(text, memory + ptr, len);
                    text[len] = '\0';
                    value = cJSON_CreateString(text);
                    free(text);
                }
            }
        } else if (!res) {
            M3ValueType type = m3_GetRetType(fn, 0);
            if (type == c_m3Type_i32 || type == c_m3Type_i64) {
                char number[32];
                snprintf(number, sizeof(number), "%lld", type == c_m3Type_i32 ?
                         (long long)result.i32 : (long long)result.i64);
                value = cJSON_CreateRaw(number);
            } else if (type == c_m3Type_f32 || type == c_m3Type_f64) {
                double number = type == c_m3Type_f32 ? result.f32 : result.f64;
                if (!isfinite(number)) res = "non-finite numeric result";
                else value = cJSON_CreateNumber(number);
            } else res = "unsupported result type";
        }
        if (!res && !value) res = "result allocation failed";
    }
    char *out = wasm_reply(res, value);
    cJSON_Delete(root);
    if (mod && !loaded) m3_FreeModule(mod);
    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    return out;
}
