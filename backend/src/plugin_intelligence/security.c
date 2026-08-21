/* security.c — static security audit. */
#include "cagent/plugin_intelligence/security.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

typedef struct rule {
    const char *pattern;
    int severity;          /* 1 low, 2 medium, 3 high */
    const char *message;
} rule;

static const rule RULES[] = {
    {"system(",       3, "direct shell execution via system()"},
    {"exec",          3, "process execution"},
    {"popen(",        3, "piped shell execution"},
    {"eval",          2, "dynamic evaluation"},
    {"rm -rf",        3, "destructive recursive delete"},
    {"rm -fr",        3, "destructive recursive delete"},
    {"chmod 777",     2, "overly permissive file mode"},
    {"strcpy(",       2, "unbounded copy (buffer overflow risk)"},
    {"strcat(",       2, "unbounded concat (buffer overflow risk)"},
    {"gets(",         3, "unsafe input (buffer overflow)"},
    {"sprintf(",      1, "unbounded format write (prefer snprintf)"},
    {"sudo",          2, "privilege elevation"},
    {"0.0.0.0",       1, "binds to all interfaces"},
    {"password",      1, "possible hard-coded credential"},
    {"api_key",       1, "possible hard-coded credential"},
};
#define N_RULES (sizeof(RULES) / sizeof(RULES[0]))

static int ci_strstr(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    size_t hlen = strlen(hay);
    if (nlen == 0 || hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (j = 0; j < nlen; j++) {
            int a = (unsigned char)hay[i + j], b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

char *ca_security_audit(const char *text) {
    cJSON *out = cJSON_CreateObject();
    if (out) {
        cJSON *findings = cJSON_CreateArray();
        for (size_t i = 0; i < N_RULES; i++) {
            if (text && ci_strstr(text, RULES[i].pattern)) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "pattern", RULES[i].pattern);
                cJSON_AddNumberToObject(o, "severity", RULES[i].severity);
                cJSON_AddStringToObject(o, "message", RULES[i].message);
                cJSON_AddItemToArray(findings, o);
            }
        }
        cJSON_AddItemToObject(out, "findings", findings);
    }
    char *s = out ? cJSON_PrintUnformatted(out) : NULL;
    if (out) cJSON_Delete(out);
    return s ? s : ca_strdup("{}");
}
