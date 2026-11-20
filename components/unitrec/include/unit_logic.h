/* The unit record: the box's own identity, kept in the last 4 KB flash sector (UNIT_ADDR), outside
 * every partition, so neither an update nor a fresh install (both write from 0x0 without a full
 * erase) reaches it. The firmware only reads it. Pure (no ESP-IDF header); host-tested in
 * test/host/test_unit_logic.c against test/vectors/unit-record-1.bin (tools/make_unit_vector.py).
 *
 * Layout (UNIT_SECTOR_SIZE bytes):
 *   0..7   magic "CDUNIT01" (ASCII)
 *   8..9   payload length, u16 little-endian, 1..UNIT_PAYLOAD_MAX
 *   10..13 CRC-32 of the payload, u32 little-endian (unit_crc32: zlib's crc32)
 *   14..15 zero (reserved, not checked)
 *   16..   payload: compact UTF-8 JSON
 *   rest   0xFF
 * Payload keys: "v" (1), "board" (12 hex digits, the chip's MAC), "caseSn" (1..32), "caseType"
 * (1..24), "edition" (1..8), "at" (ISO-8601 UTC, 1..24), "fw" (the firmware version written with
 * it). The strings are printable ASCII. Other keys (and "fw") are ignored. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define UNIT_ADDR 0xFFF000u
#define UNIT_SECTOR_SIZE 4096u
#define UNIT_HEADER_SIZE 16u
#define UNIT_PAYLOAD_MAX 1024u

typedef enum {
    UNIT_NONE,        /* no record: no magic (a blank sector reads all 0xFF) */
    UNIT_OK,          /* a record of this board's: out is filled */
    UNIT_OTHER_BOARD, /* a valid record, written for another board (a copied sector) */
    UNIT_UNREADABLE,  /* the magic, but a bad length, CRC, JSON, version or field */
} unit_status_t;

typedef struct {
    char case_sn[33];
    char case_type[25];
    char edition[9];
    char at[25];
} unit_t;

/* zlib's crc32 (IEEE 802.3, reflected, init and final xor 0xFFFFFFFF) of p[0..n). */
uint32_t unit_crc32(const uint8_t *p, size_t n);

/* Reads a record from sector[0..len) (normally the whole sector). my_board_hex: this chip's MAC as
 * 12 hex digits (either case). out is zeroed, then filled only on UNIT_OK. A field missing, of the
 * wrong type or too long makes the record UNIT_UNREADABLE (never cut short). */
unit_status_t unit_parse(const uint8_t *sector, size_t len, const char *my_board_hex, unit_t *out);
