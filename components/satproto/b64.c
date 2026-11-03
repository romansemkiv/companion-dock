#include "satproto/b64.h"

static const char ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t sat_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t olen = ((n + 2) / 3) * 4;
    if (olen + 1 > cap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = ALPHA[(v >> 18) & 63];
        out[o++] = ALPHA[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? ALPHA[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? ALPHA[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* A char's 6 bits (one lookup, not a chain of range tests); -2: '=', -1: not base64. */
static const int8_t B64_DEC[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -2, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

bool sat_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap, size_t *out_len)
{
    if (n % 4 != 0) return false;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            const int d = B64_DEC[(unsigned char)in[i + k]];
            if (d == -2) { /* '=' */
                /* padding only in last quad, positions 2/3, and contiguous */
                if (i + 4 != n || k < 2) return false;
                pad++;
                v[k] = 0;
            } else {
                if (pad) return false;
                v[k] = d;
                if (v[k] < 0) return false;
            }
        }
        size_t take = 3 - (size_t)pad;
        if (o + take > cap) return false;
        uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        out[o++] = (uint8_t)(w >> 16);
        if (take > 1) out[o++] = (uint8_t)(w >> 8);
        if (take > 2) out[o++] = (uint8_t)w;
    }
    if (out_len) *out_len = o;
    return true;
}
