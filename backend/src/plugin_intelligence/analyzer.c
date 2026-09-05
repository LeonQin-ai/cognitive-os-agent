/* analyzer.c — plugin spec analysis. */
#include "cognitive-os-agent/plugin_intelligence/analyzer.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "cJSON.h"

static int has_kw(const char *s, const char *kw) {
    if (!s) return 0;
    size_t klen = strlen(kw);
    if (klen == 0) return 0;
    const char *p = s;
    while ((p = strstr(p, kw)) != NULL) {
        p += klen;
        return 1;
    }
    return 0;
}

char *coa_analyzer_analyze(const char *spec_json) {
    cJSON *root = cJSON_Parse(spec_json ? spec_json : "{}");
    cJSON *name = root ? cJSON_GetObjectItemCaseSensitive(root, "name") : NULL;
    cJSON *desc = root ? cJSON_GetObjectItemCaseSensitive(root, "description") : NULL;
    const char *n = name && cJSON_IsString(name) ? name->valuestring : "plugin";
    const char *d = desc && cJSON_IsString(desc) ? desc->valuestring : "";

    cJSON *out = cJSON_CreateObject();
    if (out) {
        cJSON_AddStringToObject(out, "name", n);
        cJSON *caps = cJSON_CreateArray();
        cJSON *tools = cJSON_CreateArray();
        int complexity = 1;

        if (has_kw(d, "file") || has_kw(d, "read") || has_kw(d, "write")) {
            cJSON_AddItemToArray(caps, cJSON_CreateString("fs.read"));
            cJSON_AddItemToArray(caps, cJSON_CreateString("fs.write"));
            cJSON_AddItemToArray(tools, cJSON_CreateString("file_read"));
            cJSON_AddItemToArray(tools, cJSON_CreateString("file_write"));
            complexity += 1;
        }
        if (has_kw(d, "http") || has_kw(d, "network") || has_kw(d, "api") || has_kw(d, "url")) {
            cJSON_AddItemToArray(caps, cJSON_CreateString("net"));
            complexity += 2;
        }
        if (has_kw(d, "command") || has_kw(d, "shell") || has_kw(d, "exec") || has_kw(d, "script")) {
            cJSON_AddItemToArray(caps, cJSON_CreateString("proc.exec"));
            cJSON_AddItemToArray(tools, cJSON_CreateString("shell"));
            complexity += 2;
        }
        if (has_kw(d, "git") || has_kw(d, "repo") || has_kw(d, "commit")) {
            cJSON_AddItemToArray(tools, cJSON_CreateString("git"));
            complexity += 1;
        }
        if (has_kw(d, "mcp") || has_kw(d, "context")) {
            cJSON_AddItemToArray(tools, cJSON_CreateString("mcp"));
            complexity += 1;
        }
        if (complexity < 1) complexity = 1;
        if (complexity > 10) complexity = 10;

        cJSON_AddItemToObject(out, "capabilities", caps);
        cJSON_AddItemToObject(out, "tools", tools);
        cJSON_AddNumberToObject(out, "complexity", complexity);
    }

    if (root) cJSON_Delete(root);
    char *s = out ? cJSON_PrintUnformatted(out) : NULL;
    if (out) cJSON_Delete(out);
    return s ? s : coa_strdup("{}");
}
