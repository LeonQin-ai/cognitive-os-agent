/* episode.h — episodic memory: a history of (task -> result) episodes.
 * Each entry carries a wall-clock timestamp (ms since epoch) so retrieved
 * episodes can be annotated with their age. Exact-duplicate tasks update the
 * existing entry instead of adding a copy. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct episodic episodic;

episodic *episodic_new(void);
void episodic_free(episodic *e);

/* Record a completed episode (both copied; ts = ms since epoch, <=0 = now).
 * Re-experiencing an existing task REINFORCES it: strength += 1 and the
 * ts/result are refreshed. New episodes start with strength 1. */
void episodic_add(episodic *e, const char *task, const char *result);
void episodic_add_ts(episodic *e, const char *task, const char *result, long long ts);
/* Like add_ts, but also restores an explicit strength (persistence round-trip;
 * strength <= 0 falls back to 1). Dedup still reinforces (+1). */
void episodic_add_full(episodic *e, const char *task, const char *result, long long ts, double strength);
int episodic_count(episodic *e);

/* Borrowed task/result/ts of the i-th episode (0 = oldest). Do not free. */
const char *episodic_task(episodic *e, int i);
const char *episodic_result(episodic *e, int i);
long long episodic_ts(episodic *e, int i);
/* Access strength of the i-th episode (0.0 on bad args). */
double episodic_strength(episodic *e, int i);

/* Explicitly reinforce a task (+1 strength, ts refreshed). No-op if unknown. */
void episodic_reinforce(episodic *e, const char *task);

/* Lifecycle: DECAY — strength halves once per full half_life_ms elapsed since
 * the episode's ts (capped at 30 halvings), floored at floor_strength
 * (floor <= 0 = 0.001). Entries already at/below the floor are not touched.
 * Returns the number of entries decayed. */
int episodic_decay(episodic *e, long long now_ms, long long half_life_ms, double floor_strength);
/* Lifecycle: FORGET — drop episodes with strength < min_strength (compacts the
 * store). Returns the number of entries dropped. */
int episodic_drop_below(episodic *e, double min_strength);
/* Lifecycle: ARCHIVE payload — the episodes that drop_below(min_strength)
 * would remove, as a JSON array of {task,result,ts,strength} (caller frees). */
char *episodic_below_json(episodic *e, double min_strength);

/* All episodes as a JSON array of {task,result,ts,strength} (caller frees). */
char *episodic_json(episodic *e);

#ifdef __cplusplus
}
#endif
