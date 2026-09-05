/* im.h — instant messaging store (sessions + messages).
 * Sessions are persisted as JSON under <state_root>/im/sessions.json.
 * New messages are published to the event bus (COA_EV_SYSTEM, source "im") so
 * the WebSocket layer can push them to connected consoles in real time. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_im coa_im;

typedef struct coa_im_session {
    int64_t id;
    char *name;
    char *kind;           /* "direct" (default) | "group" */
    char **members;       /* member names for group sessions */
    size_t n_members;
    char *channel;        /* linked external messaging channel (NULL = none) */
    int64_t created_ms;
} coa_im_session;

typedef struct coa_im_message {
    int64_t id;
    char *role;           /* "user" | "assistant" | "system" */
    char *sender;         /* member/actor name (NULL = role only) */
    char *content;
    int64_t ts_ms;
} coa_im_message;

/* Open (or create) the IM store under <state_root>. */
coa_im *coa_im_new(const char *state_root);
void coa_im_free(coa_im *im);

/* Create a session; returns id > 0, or -1 on error. */
int64_t coa_im_create_session(coa_im *im, const char *name);

/* Create a session with a kind ("direct"|"group") and optional member names
 * (copied). Returns id > 0, or -1 on error. */
int64_t coa_im_create_session_ex(coa_im *im, const char *name, const char *kind,
                                const char **members, size_t n_members);
/* Delete a session and its messages. Returns 1 ok, 0 not found. */
int coa_im_delete_session(coa_im *im, int64_t id);

/* List sessions (malloc'd array; free with coa_im_sessions_free). */
coa_im_session *coa_im_list_sessions(coa_im *im, size_t *count);
void coa_im_sessions_free(coa_im_session *s, size_t count);

/* Messages of a session (malloc'd; free with coa_im_messages_free). */
coa_im_message *coa_im_messages(coa_im *im, int64_t session_id, size_t *count);
void coa_im_messages_free(coa_im_message *m, size_t count);

/* Append a message to a session. Returns message id > 0, -1 on error. */
int64_t coa_im_send(coa_im *im, int64_t session_id, const char *role, const char *content);

/* Append a message with an optional sender (member/actor name). */
int64_t coa_im_send_ex(coa_im *im, int64_t session_id, const char *role,
                      const char *content, const char *sender);

/* Total messages across all sessions (dashboard metric). */
size_t coa_im_total_messages(coa_im *im);

/* --- external channel linkage --- */
/* Borrowed channel name linked to a session, or NULL. */
const char *coa_im_session_channel(coa_im *im, int64_t session_id);
/* Link (or clear with NULL) an external channel to a session. Returns 0 ok. */
int coa_im_session_set_channel(coa_im *im, int64_t session_id, const char *channel);
/* First session linked to a channel, or -1. Used by the inbound poller. */
int64_t coa_im_session_by_channel(coa_im *im, const char *channel);

/* JSON snapshot {"sessions":[{id,name,kind,members,messages,...}]} (malloc'd). */
char *coa_im_sessions_json(coa_im *im);

/* History search across all sessions (case-insensitive substring on content).
 * Returns a malloc'd JSON array of matches:
 *   [{session_id,session_name,kind,id,role,sender,content,ts_ms}]
 * `limit` <= 0 means no limit. */
char *coa_im_search(coa_im *im, const char *query, int limit);

#ifdef __cplusplus
}
#endif
