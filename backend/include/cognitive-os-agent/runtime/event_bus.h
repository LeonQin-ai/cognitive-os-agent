/* event_bus.h — thread-safe publish/subscribe event bus.
 * Events: SYSTEM, TASK, MEMORY, TOOL, MODEL. Payloads are cJSON objects;
 * ownership is transferred to the bus on publish and released after dispatch. */
#pragma once
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum event_type {
    EV_SYSTEM = 0,
    EV_TASK = 1,
    EV_MEMORY = 2,
    EV_TOOL = 3,
    EV_MODEL = 4,
} event_type;

typedef struct event {
    event_type type;
    const char *source; /* borrowed, must outlive dispatch */
    int64_t ts_ms;
    cJSON *payload; /* owned by bus during dispatch */
} event;

typedef struct event_bus event_bus;

typedef void (*event_handler)(const event *ev, void *ud);

event_bus *event_bus_new(void);
void event_bus_free(event_bus *b);

/* Subscribe. type == -1 subscribes to all event types. Returns sub id. */
int event_bus_subscribe(event_bus *b, int type, event_handler fn, void *ud);

/* Publish an event. Takes ownership of payload (may be NULL). Non-blocking. */
void event_bus_publish(event_bus *b, event_type type, const char *source, cJSON *payload);
/* Convenience: publish with a JSON text payload (parsed, then freed after dispatch). */
void event_bus_publish_json(event_bus *b, event_type type, const char *source, const char *json_text);

#ifdef __cplusplus
}
#endif
