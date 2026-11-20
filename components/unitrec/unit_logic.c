#include "unit_logic.h"

#include <stdbool.h>
#include <string.h>

#include "cJSON.h"

static const char UNIT_MAGIC[8] = {'C', 'D', 'U', 'N', 'I', 'T', '0', '1'};

/* Bit by bit: the payload is read once, at start, and is at most 1 KB. */
uint32_t unit_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

static bool printable(const char *s)
{
    for (; *s; s++)
        if (*s < 0x20 || *s > 0x7E) return false;
    return true;
}

/* obj[key] as a printable ASCII string of 1..n-1 characters, copied into dst[n]. */
static bool take(const cJSON *obj, const char *key, char *dst, size_t n)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(it)) return false;
    size_t len = strlen(it->valuestring);
    if (len == 0 || len >= n || !printable(it->valuestring)) return false;
    memcpy(dst, it->valuestring, len + 1);
    return true;
}

/* cJSON recurses once per level of nesting; the record itself is one level. */
#define UNIT_DEPTH_MAX 4

/* The payload's shape, checked before cJSON sees it: an object from its first byte to its last (no
 * whitespace around it), nested at most UNIT_DEPTH_MAX deep, no control character inside a string and
 * no \u0000 (cJSON would decode it into a NUL that cuts the string short). */
static bool shape_ok(const uint8_t *p, size_t n)
{
    if (p[0] != '{' || p[n - 1] != '}') return false;
    bool in_str = false;
    int depth = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = p[i];
        if (in_str) {
            if (c < 0x20) return false;
            if (c == '"') {
                in_str = false;
            } else if (c == '\\') {
                if (n - i > 5 && memcmp(p + i + 1, "u0000", 5) == 0) return false;
                i++; /* the escaped character */
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            if (++depth > UNIT_DEPTH_MAX) return false;
        } else if (c == '}' || c == ']') {
            depth--;
        }
    }
    return true;
}

/* No name twice among obj's members (cJSON would read the first, a JSON.parse the last). */
static bool names_unique(const cJSON *obj)
{
    for (const cJSON *a = obj->child; a; a = a->next)
        for (const cJSON *b = a->next; b; b = b->next)
            if (strcmp(a->string, b->string) == 0) return false;
    return true;
}

static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* a: 12 hex digits (checked); b: anything. */
static bool same_board(const char *a, const char *b)
{
    if (strlen(b) != 12) return false;
    for (int i = 0; i < 12; i++)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

unit_status_t unit_parse(const uint8_t *sector, size_t len, const char *my_board_hex, unit_t *out)
{
    memset(out, 0, sizeof *out);
    if (len < sizeof UNIT_MAGIC || memcmp(sector, UNIT_MAGIC, sizeof UNIT_MAGIC) != 0) return UNIT_NONE;
    if (len < UNIT_HEADER_SIZE) return UNIT_UNREADABLE;
    const size_t n = (size_t)sector[8] | ((size_t)sector[9] << 8);
    if (n == 0 || n > UNIT_PAYLOAD_MAX || n > len - UNIT_HEADER_SIZE) return UNIT_UNREADABLE;
    const uint8_t *payload = sector + UNIT_HEADER_SIZE;
    const uint32_t crc = (uint32_t)sector[10] | ((uint32_t)sector[11] << 8) | ((uint32_t)sector[12] << 16) |
                         ((uint32_t)sector[13] << 24);
    if (unit_crc32(payload, n) != crc || memchr(payload, '\0', n) || !shape_ok(payload, n)) return UNIT_UNREADABLE;

    /* the JSON must take up the whole payload: nothing cut short, nothing after it */
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)payload, n, &end, false);
    bool ok = cJSON_IsObject(root) && end == (const char *)payload + n && names_unique(root);
    if (ok) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
        ok = cJSON_IsNumber(v) && v->valuedouble == 1.0;
    }
    char board[13];
    ok = ok && take(root, "board", board, sizeof board) && strlen(board) == 12;
    for (int i = 0; ok && i < 12; i++) ok = is_hex(board[i]);
    unit_t u;
    memset(&u, 0, sizeof u);
    ok = ok && take(root, "caseSn", u.case_sn, sizeof u.case_sn) &&
         take(root, "caseType", u.case_type, sizeof u.case_type) &&
         take(root, "edition", u.edition, sizeof u.edition) && take(root, "at", u.at, sizeof u.at);
    cJSON_Delete(root);
    if (!ok) return UNIT_UNREADABLE;
    if (!my_board_hex || !same_board(board, my_board_hex)) return UNIT_OTHER_BOARD;
    *out = u;
    return UNIT_OK;
}

void unit_json_add(cJSON *o, unit_status_t st, const unit_t *u)
{
    if (st == UNIT_NONE) return;
    cJSON *un = cJSON_AddObjectToObject(o, "unit");
    if (!un) return;
    if (st == UNIT_OK) {
        cJSON_AddStringToObject(un, "caseSn", u->case_sn);
        cJSON_AddStringToObject(un, "caseType", u->case_type);
        cJSON_AddStringToObject(un, "edition", u->edition);
        cJSON_AddStringToObject(un, "at", u->at);
    } else {
        cJSON_AddStringToObject(un, "error", st == UNIT_OTHER_BOARD ? "other board" : "unreadable");
    }
}
