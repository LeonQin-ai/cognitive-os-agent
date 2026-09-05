/* architect.c — plugin architecture design. */
#include "cognitive-os-agent/plugin_intelligence/architect.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static void add_component(cJSON *arr, const char *name, const char *role) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "role", role);
    cJSON_AddItemToArray(arr, o);
}

static void add_interface(cJSON *arr, const char *from, const char *to, const char *data) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "from", from);
    cJSON_AddStringToObject(o, "to", to);
    cJSON_AddStringToObject(o, "data", data);
    cJSON_AddItemToArray(arr, o);
}

char *coa_architect_design(const char *goal) {
    cJSON *out = cJSON_CreateObject();
    if (out) {
        cJSON_AddStringToObject(out, "goal", goal ? goal : "");
        cJSON *comps = cJSON_CreateArray();
        cJSON *ifaces = cJSON_CreateArray();

        add_component(comps, "ingest", "receive and normalize input");
        add_component(comps, "process", "core transformation logic");
        add_component(comps, "emit", "produce output / side effects");
        add_component(comps, "policy", "permission and capability checks");

        add_interface(ifaces, "ingest", "process", "normalized request");
        add_interface(ifaces, "process", "policy", "capability query");
        add_interface(ifaces, "process", "emit", "result");
        add_interface(ifaces, "emit", "ingest", "ack/feedback");

        cJSON_AddItemToObject(out, "components", comps);
        cJSON_AddItemToObject(out, "interfaces", ifaces);
    }
    char *s = out ? cJSON_PrintUnformatted(out) : NULL;
    if (out) cJSON_Delete(out);
    return s ? s : coa_strdup("{}");
}
