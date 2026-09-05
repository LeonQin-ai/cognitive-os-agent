/* websocket.h — RFC6455 handshake + framing primitives.
 * Standalone (no socket I/O): SHA-1 + base64 for the Sec-WebSocket-Accept
 * computation, and masked/unmasked frame build/parse. Ready to be wired into
 * the HTTP server for an Upgrade path. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- cryptographic / encoding primitives ---- */
/* FIPS 180-1 SHA-1; writes 20 bytes to out. */
void coa_sha1(const unsigned char *data, size_t len, unsigned char out[20]);
/* Base64 encode; returns malloc'd NUL-terminated string (caller frees). */
char *coa_base64_encode(const unsigned char *data, size_t len);
/* Base64 decode into out (capacity out_cap). *out_len receives decoded length.
 * Returns 0 ok, -1 on invalid input or if out_cap is too small. */
int coa_base64_decode(const char *in, unsigned char *out, size_t out_cap, size_t *out_len);

/* ---- WebSocket handshake ---- */
/* Compute Sec-WebSocket-Accept = base64(SHA1(client_key + GUID)).
 * client_key is the Sec-WebSocket-Key header value (e.g.
 * "dGhlIHNhbXBsZSBub25jZQ=="). Writes the 28-char result + NUL into out,
 * which must hold at least 29 bytes. */
void coa_ws_accept_key(const char *client_key, char out[29]);

/* ---- framing ---- */
enum {
    COA_WS_OP_TEXT   = 0x1,
    COA_WS_OP_BINARY = 0x2,
    COA_WS_OP_CLOSE  = 0x8,
    COA_WS_OP_PING   = 0x9,
    COA_WS_OP_PONG   = 0xA,
};

/* Build a single-frame message (FIN set). If mask != 0 the payload is XOR'd
 * with a fresh 4-byte key (client->server). Returns a malloc'd buffer (caller
 * frees); *out_len receives its byte length. */
char *coa_ws_build_frame(int opcode, const unsigned char *payload, size_t len,
                        int mask, size_t *out_len);

/* Parse one frame from buf[0..len). Handles masked + unmasked, 7/16/64-bit
 * lengths. Writes the unmasked payload into `payload` (caller allocates at
 * least the frame's payload size), and sets *payload_len, *opcode, *fin.
 * Returns 0 ok, -1 if truncated or an unsupported length encoding. */
int coa_ws_parse_frame(const unsigned char *buf, size_t len,
                      unsigned char *payload, size_t *payload_len,
                      int *opcode, int *fin);

#ifdef __cplusplus
}
#endif
