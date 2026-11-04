#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Standard base64 with '=' padding. Returns encoded length (NUL-terminated
 * when it fits), or 0 on overflow (needs n_out + 1 <= cap). */
size_t sat_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);

/* Returns false on illegal chars, bad padding/length or output overflow. */
bool sat_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap, size_t *out_len);

typedef enum { SAT_BITMAP_RGB, SAT_BITMAP_PNG, SAT_BITMAP_UNSUPPORTED } sat_bitmap_fmt_t;

/* KEY-STATE's BITMAP value (len bytes at v, no NUL needed): raw RGB as bare base64 (the default), or, with
 * BITMAP_FORMAT, a data URL "data:image/png;base64,<base64>". *b64 and *b64_len: the base64 part. UNSUPPORTED: a
 * data URL of another type (webp) or not in base64. */
sat_bitmap_fmt_t sat_bitmap_split(const char *v, size_t len, const char **b64, size_t *b64_len);
