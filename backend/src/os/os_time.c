#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cagent/os/os_time.h"

#include <stdio.h>
#include <time.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int64_t ca_time_now_ms(void) { return (int64_t)GetTickCount64(); }
int64_t ca_time_now_us(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)((c.QuadPart * 1000000LL) / f.QuadPart);
}

static void win_epoch_utc(struct tm *out) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100ns ticks since 1601-01-01; convert to unix seconds */
    time_t secs = (time_t)((u.QuadPart / 10000000ULL) - 11644473600ULL);
    out->tm_sec = (int)(secs % 60); secs /= 60;
    out->tm_min = (int)(secs % 60); secs /= 60;
    out->tm_hour = (int)(secs % 24); secs /= 24;
    /* civil_from_days */
    int z = (int)secs + 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    out->tm_year = y - 1900;
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_wday = 0;
    out->tm_yday = (int)doy;
    out->tm_isdst = 0;
}

static void fill_utc(struct tm *out) {
#if defined(_WIN32)
    win_epoch_utc(out);
#else
    time_t now = time(NULL);
    gmtime_r(&now, out);
#endif
}

void ca_time_now_str(char *out, size_t n) {
    struct tm tmv;
    fill_utc(&tmv);
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

void ca_time_now_iso(char *out, size_t n) {
    struct tm tmv;
    fill_utc(&tmv);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

#else /* POSIX */

#include <sys/time.h>
#include <unistd.h>

int64_t ca_time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int64_t ca_time_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void fill_utc(struct tm *out) {
    time_t now = time(NULL);
    gmtime_r(&now, out);
}

void ca_time_now_str(char *out, size_t n) {
    struct tm tmv;
    fill_utc(&tmv);
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

void ca_time_now_iso(char *out, size_t n) {
    struct tm tmv;
    fill_utc(&tmv);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

#endif

void ca_time_sleep_ms(int ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}
