/* channel.h — external messaging channel adapters (IM bridge).
 *
 * Bridges cognitive-os-agent IM out to mainstream chat platforms so conversations can
 * reach a phone. Channel types:
 *   feishu   — 飞书自定义机器人 webhook:  {"msg_type":"text","content":{"text":...}}
 *   wecom    — 企业微信群机器人 webhook:   {"msgtype":"text","text":{"content":...}}
 *   generic  — 任意 JSON webhook:          {"text":...}
 *   telegram — Telegram Bot API:           POST {endpoint}/bot{token}/sendMessage
 *              plus an inbound poller (getUpdates) so phone messages flow back
 *              into the linked IM session.
 *
 * HTTPS endpoints require a local TLS-terminating proxy (the bundled HTTP
 * client is plain-TCP; see build/llm_proxy.js for the established pattern).
 * Channels persist to <state_root>/im/channels.json. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_im_channels coa_im_channels;

typedef struct coa_im_channel {
    char *name;
    char *type;            /* feishu | wecom | generic | telegram */
    char *endpoint;        /* webhook url, or telegram API base (http(s)://host) */
    char *token;           /* telegram bot token */
    char *target;          /* telegram chat_id (the phone-side chat) */
    int enabled;
    int64_t last_update_id; /* telegram poll watermark (internal) */
} coa_im_channel;

/* Open (or create) the channel registry under <state_root>/im/channels.json. */
coa_im_channels *coa_im_channels_new(const char *state_root);
void coa_im_channels_free(coa_im_channels *cs);

/* Register (overwrite same name). Copies all fields. Returns 0 ok, -1 error. */
int coa_im_channel_register(coa_im_channels *cs, const coa_im_channel *ch);
int coa_im_channel_remove(coa_im_channels *cs, const char *name);
/* Borrowed lookup (valid until next mutation); callers may read, and the
 * telegram poller writes the internal last_update_id watermark. */
coa_im_channel *coa_im_channel_find(coa_im_channels *cs, const char *name);
int coa_im_channel_count(coa_im_channels *cs);
coa_im_channel *coa_im_channel_get(coa_im_channels *cs, size_t i);

/* Send `text` through a channel. Returns malloc'd JSON:
 *   {"ok":true,"channel":name,"response":"..."} | {"ok":false,"error":"..."} */
char *coa_im_channel_send(coa_im_channels *cs, const char *name, const char *text);

/* JSON snapshot {"channels":[{name,type,endpoint,token,target,enabled}]} */
char *coa_im_channels_json(coa_im_channels *cs);

/* Telegram inbound: poll getUpdates once for a channel and ingest new text
 * messages via `ingest(channel_name, sender, text, ud)`. Returns # ingested,
 * or -1 if the channel is missing/not telegram/unreachable. */
typedef void (*coa_im_ingest_fn)(const char *channel_name, const char *sender,
                                const char *text, void *ud);
int coa_im_channel_poll_telegram(coa_im_channels *cs, const char *name,
                                coa_im_ingest_fn ingest, void *ud);

#ifdef __cplusplus
}
#endif
