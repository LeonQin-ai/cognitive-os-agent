/* event_bus.h — thread-safe publish/subscribe event bus.
 * Events: SYSTEM, TASK, MEMORY, TOOL, MODEL. Payloads are cJSON objects;
 * ownership is transferred to the bus on publish and released after dispatch. */
#pragma once
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ca_event_type {
    CA_EV_SYSTEM = 0,
    CA_EV_TASK   = 1,
    CA_EV_MEMORY = 2,
    CA_EV_TOOL   = 3,
    CA_EV_MODEL  = 4,
} ca_event_type;

typedef struct ca_event {
    ca_event_type type;
    const char *source;  /* borrowed, must outlive dispatch */
    int64_t ts_ms;
    cJSON *payload;      /* owned by bus during dispatch */
} ca_event;

typedef struct ca_event_bus ca_event_bus;

typedef void (*ca_event_handler)(const ca_event *ev, void *ud);

ca_event_bus *ca_event_bus_new(void);
void ca_event_bus_free(ca_event_bus *b);

/* Subscribe. type == -1 subscribes to all event types. Returns sub id. */
int ca_event_bus_subscribe(ca_event_bus *b, int type, ca_event_handler fn, void *ud);

/* Publish an event. Takes ownership of payload (may be NULL). Non-blocking. */
void ca_event_bus_publish(ca_event_bus *b, ca_event_type type, const char *source, cJSON *payload);
/* Convenience: publish with a JSON text payload (parsed, then freed after dispatch). */
void ca_event_bus_publish_json(ca_event_bus *b, ca_event_type type, const char *source, const char *json_text);

#ifdef __cplusplus
}
#endif
