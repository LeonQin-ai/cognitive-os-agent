/* os_time.h — monotonic clock, wall clock, sleep */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic milliseconds since an arbitrary epoch (for measuring durations). */
int64_t ca_time_now_ms(void);
/* Monotonic microseconds since an arbitrary epoch. */
int64_t ca_time_now_us(void);

/* Current wall clock as "YYYY-MM-DD HH:MM:SS" into out[0..n). */
void ca_time_now_str(char *out, size_t n);

/* Wall-clock ISO8601 timestamp with ms: "YYYY-MM-DDTHH:MM:SS.mmmZ". */
void ca_time_now_iso(char *out, size_t n);

void ca_time_sleep_ms(int ms);

#ifdef __cplusplus
}
#endif
