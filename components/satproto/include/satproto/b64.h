#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Standard base64 with '=' padding. Returns encoded length (NUL-terminated
 * when it fits), or 0 on overflow (needs n_out + 1 <= cap). */
size_t sat_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);

/* Returns false on illegal chars, bad padding/length or output overflow. */
bool sat_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap, size_t *out_len);
