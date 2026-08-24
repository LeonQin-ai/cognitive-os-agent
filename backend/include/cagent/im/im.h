/* im.h — instant messaging store (sessions + messages).
 * Sessions are persisted as JSON under <state_root>/im/sessions.json.
 * New messages are published to the event bus (CA_EV_SYSTEM, source "im") so
 * the WebSocket layer can push them to connected consoles in real time. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_im ca_im;

typedef struct ca_im_session {
    int64_t id;
    char *name;
    int64_t created_ms;
} ca_im_session;

typedef struct ca_im_message {
    int64_t id;
    char *role;       /* "user" | "assistant" | "system" */
    char *content;
    int64_t ts_ms;
} ca_im_message;

/* Open (or create) the IM store under <state_root>. */
ca_im *ca_im_new(const char *state_root);
void ca_im_free(ca_im *im);

/* Create a session; returns id > 0, or -1 on error. */
int64_t ca_im_create_session(ca_im *im, const char *name);
/* Delete a session and its messages. Returns 1 ok, 0 not found. */
int ca_im_delete_session(ca_im *im, int64_t id);

/* List sessions (malloc'd array; free with ca_im_sessions_free). */
ca_im_session *ca_im_list_sessions(ca_im *im, size_t *count);
void ca_im_sessions_free(ca_im_session *s, size_t count);

/* Messages of a session (malloc'd; free with ca_im_messages_free). */
ca_im_message *ca_im_messages(ca_im *im, int64_t session_id, size_t *count);
void ca_im_messages_free(ca_im_message *m, size_t count);

/* Append a message to a session. Returns message id > 0, -1 on error. */
int64_t ca_im_send(ca_im *im, int64_t session_id, const char *role, const char *content);

/* Total messages across all sessions (dashboard metric). */
size_t ca_im_total_messages(ca_im *im);

/* JSON snapshot {"sessions":[{id,name,created_ms,messages:[...]}]} (malloc'd). */
char *ca_im_sessions_json(ca_im *im);

#ifdef __cplusplus
}
#endif
