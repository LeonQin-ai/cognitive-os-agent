/* im.h — instant messaging store (sessions + messages).
 * Sessions are persisted as JSON under <state_root>/im/sessions.json.
 * New messages are published to the event bus (EV_SYSTEM, source "im") so
 * the WebSocket layer can push them to connected consoles in real time. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct im im;

typedef struct im_session {
    int64_t id;
    char *name;
    char *kind;     /* "direct" (default) | "group" */
    char **members; /* member names for group sessions */
    size_t n_members;
    char *channel; /* linked external messaging channel (NULL = none) */
    int64_t created_ms;
} im_session;

typedef struct im_message {
    int64_t id;
    char *role;   /* "user" | "assistant" | "system" */
    char *sender; /* member/actor name (NULL = role only) */
    char *content;
    int64_t ts_ms;
} im_message;

/* Open (or create) the IM store under <state_root>. */
im *im_new(const char *state_root);
void im_free(im *im);

/* Create a session; returns id > 0, or -1 on error. */
int64_t im_create_session(im *im, const char *name);

/* Create a session with a kind ("direct"|"group") and optional member names
 * (copied). Returns id > 0, or -1 on error. */
int64_t im_create_session_ex(im *im, const char *name, const char *kind, const char **members,
                                 size_t n_members);
/* Delete a session and its messages. Returns 1 ok, 0 not found. */
int im_delete_session(im *im, int64_t id);

/* List sessions (malloc'd array; free with im_sessions_free). */
im_session *im_list_sessions(im *im, size_t *count);
void im_sessions_free(im_session *s, size_t count);

/* Messages of a session (malloc'd; free with im_messages_free). */
im_message *im_messages(im *im, int64_t session_id, size_t *count);
void im_messages_free(im_message *m, size_t count);

/* Append a message to a session. Returns message id > 0, -1 on error. */
int64_t im_send(im *im, int64_t session_id, const char *role, const char *content);

/* Append a message with an optional sender (member/actor name). */
int64_t im_send_ex(im *im, int64_t session_id, const char *role, const char *content, const char *sender);

/* Total messages across all sessions (dashboard metric). */
size_t im_total_messages(im *im);

/* --- external channel linkage --- */
/* Borrowed channel name linked to a session, or NULL. */
const char *im_session_channel(im *im, int64_t session_id);
/* Link (or clear with NULL) an external channel to a session. Returns 0 ok. */
int im_session_set_channel(im *im, int64_t session_id, const char *channel);
/* First session linked to a channel, or -1. Used by the inbound poller. */
int64_t im_session_by_channel(im *im, const char *channel);

/* JSON snapshot {"sessions":[{id,name,kind,members,messages,...}]} (malloc'd). */
char *im_sessions_json(im *im);

/* History search across all sessions (case-insensitive substring on content).
 * Returns a malloc'd JSON array of matches:
 *   [{session_id,session_name,kind,id,role,sender,content,ts_ms}]
 * `limit` <= 0 means no limit. */
char *im_search(im *im, const char *query, int limit);

#ifdef __cplusplus
}
#endif
