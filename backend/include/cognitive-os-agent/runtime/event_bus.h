/* event_bus.h — thread-safe publish/subscribe event bus.
 * Events: SYSTEM, TASK, MEMORY, TOOL, MODEL. Payloads are cJSON objects;
 * ownership is transferred to the bus on publish and released after dispatch. */
#pragma once
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum coa_event_type {
    COA_EV_SYSTEM = 0,
    COA_EV_TASK   = 1,
    COA_EV_MEMORY = 2,
    COA_EV_TOOL   = 3,
    COA_EV_MODEL  = 4,
} coa_event_type;

typedef struct coa_event {
    coa_event_type type;
    const char *source;  /* borrowed, must outlive dispatch */
    int64_t ts_ms;
    cJSON *payload;      /* owned by bus during dispatch */
} coa_event;

typedef struct coa_event_bus coa_event_bus;

typedef void (*coa_event_handler)(const coa_event *ev, void *ud);

coa_event_bus *coa_event_bus_new(void);
void coa_event_bus_free(coa_event_bus *b);

/* Subscribe. type == -1 subscribes to all event types. Returns sub id. */
int coa_event_bus_subscribe(coa_event_bus *b, int type, coa_event_handler fn, void *ud);

/* Publish an event. Takes ownership of payload (may be NULL). Non-blocking. */
void coa_event_bus_publish(coa_event_bus *b, coa_event_type type, const char *source, cJSON *payload);
/* Convenience: publish with a JSON text payload (parsed, then freed after dispatch). */
void coa_event_bus_publish_json(coa_event_bus *b, coa_event_type type, const char *source, const char *json_text);

#ifdef __cplusplus
}
#endif
