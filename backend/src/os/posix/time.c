#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os/os_time.h"

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

/* Wall-clock epoch ms — NOT uptime (see the Windows side comment). Persisted
 * timestamps (episodes, audit, state) must stay meaningful across reboots. */
int64_t time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int64_t time_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void fill_utc(struct tm *out) {
    time_t now = time(NULL);
    gmtime_r(&now, out);
}

void time_now_str(char *out, size_t n) {
    struct tm tmv;
    fill_utc(&tmv);
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
             tmv.tm_min, tmv.tm_sec);
}

void time_now_iso(char *out, size_t n) {
    struct tm tmv;
    fill_utc(&tmv);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.000Z", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
             tmv.tm_min, tmv.tm_sec);
}

void time_sleep_ms(int ms) {
    usleep((useconds_t)ms * 1000);
}
