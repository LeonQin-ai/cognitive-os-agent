#pragma once
/* Task-scoped cancellation; thread-local so another chat cannot cancel this
 * request. Platform watchers interrupt blocked native I/O and are joined before
 * releasing the borrowed callback context or transport handle. */
typedef int (*http_cancel_check)(void *context);
void http_set_cancel_check(http_cancel_check check,void *context);
/* Borrow the current binding so nested runs can restore their caller's scope. */
http_cancel_check http_get_cancel_check(void **context);
int http_cancel_requested(void);
typedef struct http_cancel_guard http_cancel_guard;
http_cancel_guard *http_cancel_watch(void (*interrupt)(void *),void *handle);
int http_cancel_unwatch(http_cancel_guard *guard); /* whether interrupt ran */

