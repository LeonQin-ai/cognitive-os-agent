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
    char *kind;           /* "direct" (default) | "group" */
    char **members;       /* member names for group sessions */
    size_t n_members;
    char *channel;        /* linked external messaging channel (NULL = none) */
    int64_t created_ms;
} ca_im_session;

typedef struct ca_im_message {
    int64_t id;
    char *role;           /* "user" | "assistant" | "system" */
    char *sender;         /* member/actor name (NULL = role only) */
    char *content;
    int64_t ts_ms;
} ca_im_message;

/* Open (or create) the IM store under <state_root>. */
ca_im *ca_im_new(const char *state_root);
void ca_im_free(ca_im *im);

/* Create a session; returns id > 0, or -1 on error. */
int64_t ca_im_create_session(ca_im *im, const char *name);

/* Create a session with a kind ("direct"|"group") and optional member names
 * (copied). Returns id > 0, or -1 on error. */
int64_t ca_im_create_session_ex(ca_im *im, const char *name, const char *kind,
                                const char **members, size_t n_members);
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

/* Append a message with an optional sender (member/actor name). */
int64_t ca_im_send_ex(ca_im *im, int64_t session_id, const char *role,
                      const char *content, const char *sender);

/* Total messages across all sessions (dashboard metric). */
size_t ca_im_total_messages(ca_im *im);

/* --- external channel linkage --- */
/* Borrowed channel name linked to a session, or NULL. */
const char *ca_im_session_channel(ca_im *im, int64_t session_id);
/* Link (or clear with NULL) an external channel to a session. Returns 0 ok. */
int ca_im_session_set_channel(ca_im *im, int64_t session_id, const char *channel);
/* First session linked to a channel, or -1. Used by the inbound poller. */
int64_t ca_im_session_by_channel(ca_im *im, const char *channel);

/* JSON snapshot {"sessions":[{id,name,kind,members,messages,...}]} (malloc'd). */
char *ca_im_sessions_json(ca_im *im);

/* History search across all sessions (case-insensitive substring on content).
 * Returns a malloc'd JSON array of matches:
 *   [{session_id,session_name,kind,id,role,sender,content,ts_ms}]
 * `limit` <= 0 means no limit. */
char *ca_im_search(ca_im *im, const char *query, int limit);

#ifdef __cplusplus
}
#endif
