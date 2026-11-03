#pragma once
/* The image reports' header lengths and the little-endian 16-bit read, one copy for the writers (images.c), their
 * inverse (parse.c, which dockcache uses on Companion's reports) and the input parser (they must agree).
 * Private to deckproto. */
#include <stdint.h>

#define GEN2_KEY_HDR 8
#define GEN2_LCD_HDR 16
#define GEN1_HDR 16

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
