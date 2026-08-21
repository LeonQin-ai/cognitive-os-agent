/* websocket.c — RFC6455 handshake + framing primitives. */
#include "cagent/api/websocket.h"
#include "cagent/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* ================= SHA-1 (FIPS 180-1) ================= */

static uint32_t rol32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(uint32_t h[5], const unsigned char *p) {
    uint32_t w[80];
    int t;
    for (t = 0; t < 16; t++)
        w[t] = ((uint32_t)p[t * 4] << 24) | ((uint32_t)p[t * 4 + 1] << 16) |
               ((uint32_t)p[t * 4 + 2] << 8) | (uint32_t)p[t * 4 + 3];
    for (t = 16; t < 80; t++)
        w[t] = rol32(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (t = 0; t < 80; t++) {
        uint32_t f, k;
        if (t < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999u; }
        else if (t < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[t];
        e = d; d = c; c = rol32(b, 30); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

typedef struct {
    uint32_t h[5];
    uint64_t total;
    unsigned char block[64];
    size_t block_len;
} sha1_ctx;

static void sha1_init(sha1_ctx *s) {
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
    s->total = 0;
    s->block_len = 0;
}

static void sha1_update(sha1_ctx *s, const unsigned char *data, size_t len) {
    s->total += len;
    while (len > 0) {
        size_t space = 64 - s->block_len;
        size_t n = len < space ? len : space;
        memcpy(s->block + s->block_len, data, n);
        s->block_len += n;
        data += n;
        len -= n;
        if (s->block_len == 64) {
            sha1_block(s->h, s->block);
            s->block_len = 0;
        }
    }
}

static void sha1_final(sha1_ctx *s, unsigned char out[20]) {
    uint64_t bitlen = s->total * 8;
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    sha1_update(s, &pad, 1);
    while (s->block_len != 56) sha1_update(s, &zero, 1);
    unsigned char lenbuf[8];
    int i;
    for (i = 0; i < 8; i++) lenbuf[i] = (unsigned char)(bitlen >> (56 - 8 * i));
    sha1_update(s, lenbuf, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(s->h[i]);
    }
}

void ca_sha1(const unsigned char *data, size_t len, unsigned char out[20]) {
    sha1_ctx s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    sha1_final(&s, out);
}

/* ================= base64 ================= */

static const char b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *ca_base64_encode(const unsigned char *data, size_t len) {
    size_t olen = ((len + 2) / 3) * 4;
    char *out = (char *)malloc(olen + 1);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = b64_tab[(v >> 18) & 63];
        out[o++] = b64_tab[(v >> 12) & 63];
        out[o++] = b64_tab[(v >> 6) & 63];
        out[o++] = b64_tab[v & 63];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        out[o++] = b64_tab[(v >> 18) & 63];
        out[o++] = b64_tab[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = b64_tab[(v >> 18) & 63];
        out[o++] = b64_tab[(v >> 12) & 63];
        out[o++] = b64_tab[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int ca_base64_decode(const char *in, unsigned char *out, size_t out_cap, size_t *out_len) {
    size_t ilen = strlen(in);
    size_t olen = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < ilen; i++) {
        char c = in[i];
        if (c == '=' || c == '\n' || c == '\r') {
            if (c == '=') continue; /* padding handled by bit count */
            continue;
        }
        int v = b64_val(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            unsigned char b = (unsigned char)((acc >> bits) & 0xFF);
            if (olen >= out_cap) return -1;
            out[olen++] = b;
        }
    }
    if (out_len) *out_len = olen;
    return 0;
}

/* ================= WebSocket ================= */

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void ca_ws_accept_key(const char *client_key, char out[29]) {
    char buf[256];
    unsigned char sha[20];
    snprintf(buf, sizeof(buf), "%s%s", client_key ? client_key : "", WS_GUID);
    ca_sha1((const unsigned char *)buf, strlen(buf), sha);
    char *b64 = ca_base64_encode(sha, 20);
    snprintf(out, 29, "%s", b64 ? b64 : "");
    free(b64);
}

char *ca_ws_build_frame(int opcode, const unsigned char *payload, size_t len,
                        int mask, size_t *out_len) {
    size_t header = 2;
    if (len >= 126) header += 2;
    if (len >= 65536) header += 8;
    if (mask) header += 4;

    unsigned char *buf = (unsigned char *)malloc(header + len);
    if (!buf) return NULL;

    buf[0] = (unsigned char)(0x80 | (opcode & 0x0F)); /* FIN=1 */

    size_t off;
    if (len < 126) {
        buf[1] = (unsigned char)((mask ? 0x80 : 0) | len);
        off = 2;
    } else if (len < 65536) {
        buf[1] = (unsigned char)((mask ? 0x80 : 0) | 126);
        buf[2] = (unsigned char)((len >> 8) & 0xFF);
        buf[3] = (unsigned char)(len & 0xFF);
        off = 4;
    } else {
        buf[1] = (unsigned char)((mask ? 0x80 : 0) | 127);
        int i;
        for (i = 0; i < 8; i++) buf[2 + i] = (unsigned char)((len >> (56 - 8 * i)) & 0xFF);
        off = 10;
    }

    if (mask) {
        unsigned char key[4];
        uint64_t seed = (uint64_t)ca_time_now_us();
        int i;
        for (i = 0; i < 4; i++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            key[i] = (unsigned char)(seed & 0xFF);
        }
        memcpy(buf + off, key, 4);
        off += 4;
        for (size_t j = 0; j < len; j++) buf[off + j] = payload[j] ^ key[j & 3];
    } else {
        memcpy(buf + off, payload, len);
    }

    if (out_len) *out_len = header + len;
    return (char *)buf;
}

int ca_ws_parse_frame(const unsigned char *buf, size_t len,
                      unsigned char *payload, size_t *payload_len,
                      int *opcode, int *fin) {
    if (len < 2) return -1;
    int f = (buf[0] >> 7) & 1;
    int op = buf[0] & 0x0F;
    int masked = (buf[1] >> 7) & 1;
    size_t plen = buf[1] & 0x7F;
    size_t off = 2;

    if (plen == 126) {
        if (len < off + 2) return -1;
        plen = ((size_t)buf[off] << 8) | buf[off + 1];
        off += 2;
    } else if (plen == 127) {
        if (len < off + 8) return -1;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | buf[off + i];
        off += 8;
    }

    unsigned char key[4] = {0, 0, 0, 0};
    if (masked) {
        if (len < off + 4) return -1;
        memcpy(key, buf + off, 4);
        off += 4;
    }

    if (len < off + plen) return -1;
    for (size_t i = 0; i < plen; i++)
        payload[i] = masked ? (buf[off + i] ^ key[i & 3]) : buf[off + i];

    if (payload_len) *payload_len = plen;
    if (opcode) *opcode = op;
    if (fin) *fin = f;
    return 0;
}
