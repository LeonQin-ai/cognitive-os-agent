/* cron.c — scheduled tasks (定时任务). See cron.h. */
#include "runtime/cron.h"
#include "runtime/scheduler.h"
#include "infra/util.h"
#include "infra/logging.h"
#include "os/os_time.h"
#include "os/os_thread.h"
#include "os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "cJSON.h"

#define CRON_TICK_MS 15000      /* background tick period */
#define CRON_NAME_MAX 64
#define CRON_PROMPT_MAX 1024
#define CRON_SESSION_MAX 64
#define CRON_AT_MAX 8
#define CRON_MAX_JOBS 64
#define CRON_DEFAULT_SESSION "cron"

typedef struct {
    long long id;
    char name[CRON_NAME_MAX];
    char prompt[CRON_PROMPT_MAX];
    char session[CRON_SESSION_MAX];
    int every_sec;              /* > 0 = interval job */
    char at[CRON_AT_MAX];       /* "HH:MM" = daily job */
    int enabled;
    long long last_run_ms;
    long long next_run_ms;
} cron_job;

struct cron_mgr {
    mutex_t mtx;
    scheduler *sched;
    char state_root[512];
    char path[600];             /* <state_root>/cron.json */
    cron_job jobs[CRON_MAX_JOBS];
    size_t n;
    long long next_id;
    volatile int stop;
    struct thread_t *tick;
};

/* ---- persistence ---- */

static void cron_save_locked(cron_mgr *c) {
    cJSON *arr, *o;
    char *s;
    FILE *f;

    arr = cJSON_CreateArray();
    if (!arr)
        return;
    for (size_t i = 0; i < c->n; i++) {
        cron_job *j = &c->jobs[i];
        o = cJSON_CreateObject();
        if (!o)
            break;
        cJSON_AddNumberToObject(o, "id", (double)j->id);
        cJSON_AddStringToObject(o, "name", j->name);
        cJSON_AddStringToObject(o, "prompt", j->prompt);
        cJSON_AddStringToObject(o, "session", j->session);
        cJSON_AddNumberToObject(o, "every_sec", j->every_sec);
        if (j->at[0])
            cJSON_AddStringToObject(o, "at", j->at);
        cJSON_AddBoolToObject(o, "enabled", j->enabled ? 1 : 0);
        cJSON_AddNumberToObject(o, "last_run_ms", (double)j->last_run_ms);
        cJSON_AddNumberToObject(o, "next_run_ms", (double)j->next_run_ms);
        cJSON_AddItemToArray(arr, o);
    }
    s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s)
        return;
    f = fopen(c->path, "wb");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
    free(s);
}

static void cron_load(cron_mgr *c) {
    char *buf;
    long len;
    FILE *f;
    cJSON *root, *it;

    f = fopen(c->path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 512 * 1024) {
        fclose(f);
        return;
    }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';

    root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsArray(root)) {
        if (root)
            cJSON_Delete(root);
        return;
    }
    cJSON_ArrayForEach(it, root) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
        cJSON *pr = cJSON_GetObjectItemCaseSensitive(it, "prompt");
        cJSON *se = cJSON_GetObjectItemCaseSensitive(it, "session");
        cJSON *ev = cJSON_GetObjectItemCaseSensitive(it, "every_sec");
        cJSON *at = cJSON_GetObjectItemCaseSensitive(it, "at");
        cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
        cron_job *j;

        if (c->n == CRON_MAX_JOBS)
            break;
        if (!cJSON_IsString(pr) || !pr->valuestring || !*pr->valuestring)
            continue;
        j = &c->jobs[c->n++];
        memset(j, 0, sizeof(*j));
        j->id = (jid && cJSON_IsNumber(jid)) ? (long long)jid->valuedouble : 0;
        if (nm && cJSON_IsString(nm) && nm->valuestring)
            snprintf(j->name, sizeof(j->name), "%s", nm->valuestring);
        snprintf(j->prompt, sizeof(j->prompt), "%s", pr->valuestring);
        if (se && cJSON_IsString(se) && se->valuestring)
            snprintf(j->session, sizeof(j->session), "%s", se->valuestring);
        if (ev && cJSON_IsNumber(ev))
            j->every_sec = (int)ev->valuedouble;
        if (at && cJSON_IsString(at) && at->valuestring)
            snprintf(j->at, sizeof(j->at), "%s", at->valuestring);
        j->enabled = en ? cJSON_IsTrue(en) : 1;
        j->next_run_ms = 0; /* due immediately after restart */
        if (j->id >= c->next_id)
            c->next_id = j->id + 1;
    }
    cJSON_Delete(root);
}

/* ---- scheduling ---- */

/* Next daily "HH:MM" occurrence (local time), strictly in the future. */
static long long daily_next_ms(const char *at) {
    int hh = 0, mm = 0;
    time_t now_t;
    struct tm lt;
    long long candidate;

    if (!at || sscanf(at, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59)
        return 0;
    time(&now_t);
    lt = *localtime(&now_t);
    lt.tm_hour = hh;
    lt.tm_min = mm;
    lt.tm_sec = 0;
    candidate = (long long)mktime(&lt) * 1000;
    if (candidate <= time_now_ms())
        candidate += 24LL * 3600 * 1000;
    return candidate;
}

static void job_schedule_next(cron_job *j, long long now_ms) {
    if (j->every_sec > 0)
        j->next_run_ms = now_ms + (long long)j->every_sec * 1000;
    else
        j->next_run_ms = daily_next_ms(j->at);
}

int cron_tick(cron_mgr *c) {
    int submitted = 0;
    long long now = time_now_ms();

    if (!c)
        return 0;
    mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->n; i++) {
        cron_job *j = &c->jobs[i];
        if (!j->enabled)
            continue;
        if (j->next_run_ms == 0)
            job_schedule_next(j, now); /* first schedule on load/add */
        if (j->next_run_ms > now)
            continue;
        if (c->sched) {
            char session[CRON_SESSION_MAX];
            snprintf(session, sizeof(session), "%s", j->session[0] ? j->session : CRON_DEFAULT_SESSION);
            int64_t tid = scheduler_submit_tag(c->sched, 0, j->prompt, NULL, 0, session);
            if (tid >= 0)
                log_info("cron: submitted job '%s' (task %lld, session %s)", j->name, (long long)tid, session);
        }
        j->last_run_ms = now;
        job_schedule_next(j, now);
        submitted++;
    }
    cron_save_locked(c);
    mutex_unlock(&c->mtx);
    return submitted;
}

static void cron_loop(void *arg) {
    cron_mgr *c = (cron_mgr *)arg;

    while (!c->stop) {
        cron_tick(c);
        for (int waited = 0; waited < CRON_TICK_MS && !c->stop; waited += 200)
            time_sleep_ms(200);
    }
}

/* ---- public API ---- */

cron_mgr *cron_new(scheduler *sched, const char *state_root) {
    cron_mgr *c = (cron_mgr *)calloc(1, sizeof(cron_mgr));

    if (!c)
        return NULL;
    c->sched = sched;
    snprintf(c->state_root, sizeof(c->state_root), "%s", state_root ? state_root : "state");
    path_join(c->path, sizeof(c->path), c->state_root, "cron.json");
    mutex_init(&c->mtx);
    cron_load(c);
    return c;
}

void cron_free(cron_mgr *c) {
    if (!c)
        return;
    c->stop = 1;
    if (c->tick) {
        thread_join(c->tick);
        c->tick = NULL;
    }
    mutex_destroy(&c->mtx);
    free(c);
}

int cron_start(cron_mgr *c) {
    if (!c || c->tick)
        return -1;
    c->tick = thread_create(cron_loop, c);
    return c->tick ? 0 : -1;
}

long long cron_add(cron_mgr *c, const char *name, const char *prompt, const char *session, int every_sec,
                   const char *at) {
    long long id = -1;

    if (!c || !prompt || !*prompt)
        return -1;
    if (every_sec <= 0 && (!at || !*at))
        return -1;
    if (at && *at) {
        int hh = 0, mm = 0;
        if (sscanf(at, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59)
            return -1;
    }
    mutex_lock(&c->mtx);
    if (c->n < CRON_MAX_JOBS) {
        cron_job *j = &c->jobs[c->n++];
        memset(j, 0, sizeof(*j));
        id = j->id = c->next_id++;
        snprintf(j->name, sizeof(j->name), "%s", (name && *name) ? name : "job");
        snprintf(j->prompt, sizeof(j->prompt), "%.*s", CRON_PROMPT_MAX - 1, prompt);
        snprintf(j->session, sizeof(j->session), "%s", (session && *session) ? session : CRON_DEFAULT_SESSION);
        j->every_sec = every_sec;
        if (at && *at)
            snprintf(j->at, sizeof(j->at), "%s", at);
        j->enabled = 1;
        j->next_run_ms = 0;
        cron_save_locked(c);
    }
    mutex_unlock(&c->mtx);
    return id;
}

int cron_remove(cron_mgr *c, long long id) {
    int rc = -1;

    if (!c)
        return -1;
    mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->n; i++) {
        if (c->jobs[i].id == id) {
            memmove(&c->jobs[i], &c->jobs[i + 1], (c->n - i - 1) * sizeof(cron_job));
            c->n--;
            rc = 0;
            cron_save_locked(c);
            break;
        }
    }
    mutex_unlock(&c->mtx);
    return rc;
}

int cron_set_enabled(cron_mgr *c, long long id, int enabled) {
    int rc = -1;

    if (!c)
        return -1;
    mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->n; i++) {
        if (c->jobs[i].id == id) {
            c->jobs[i].enabled = enabled ? 1 : 0;
            rc = 0;
            cron_save_locked(c);
            break;
        }
    }
    mutex_unlock(&c->mtx);
    return rc;
}

char *cron_json(const cron_mgr *c) {
    cJSON *arr, *o;
    char *s;

    if (!c)
        return xstrdup("[]");
    mutex_lock(&((cron_mgr *)c)->mtx);
    arr = cJSON_CreateArray();
    for (size_t i = 0; i < c->n; i++) {
        const cron_job *j = &c->jobs[i];
        o = cJSON_CreateObject();
        if (!o)
            break;
        cJSON_AddNumberToObject(o, "id", (double)j->id);
        cJSON_AddStringToObject(o, "name", j->name);
        cJSON_AddStringToObject(o, "prompt", j->prompt);
        cJSON_AddStringToObject(o, "session", j->session);
        cJSON_AddNumberToObject(o, "every_sec", j->every_sec);
        if (j->at[0])
            cJSON_AddStringToObject(o, "at", j->at);
        cJSON_AddBoolToObject(o, "enabled", j->enabled ? 1 : 0);
        cJSON_AddNumberToObject(o, "last_run_ms", (double)j->last_run_ms);
        cJSON_AddNumberToObject(o, "next_run_ms", (double)j->next_run_ms);
        cJSON_AddItemToArray(arr, o);
    }
    s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    mutex_unlock(&((cron_mgr *)c)->mtx);
    return s ? s : xstrdup("[]");
}
