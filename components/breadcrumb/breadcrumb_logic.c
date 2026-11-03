#include "breadcrumb_logic.h"

#include <stdio.h>
#include <string.h>

/* "BCRM", with the record's size in the top byte: another layout never reads as this one. */
#define BC_MAGIC (0x4243524du ^ ((uint32_t)sizeof(bc_record_t) << 24))

/* FNV-1a over everything before the checksum. */
static uint32_t bc_sum(const bc_record_t *r)
{
    const unsigned char *p = (const unsigned char *)r;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(bc_record_t, sum); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

void bc_encode(bc_record_t *r, const char *reason, bc_stage_t stage)
{
    memset(r, 0, sizeof *r); /* the bytes after the NUL too: the checksum covers them */
    r->magic = BC_MAGIC;
    r->stage = (uint32_t)stage;
    size_t n = 0;
    for (const char *s = reason ? reason : ""; *s && n < BC_REASON_MAX - 1; s++)
        if (*s >= 0x20 && *s < 0x7F) r->reason[n++] = *s;
    r->sum = bc_sum(r);
}

bool bc_valid(const bc_record_t *r)
{
    return r->magic == BC_MAGIC && (r->stage == BC_STAGE_SCHEDULED || r->stage == BC_STAGE_RESTARTING) &&
           memchr(r->reason, '\0', sizeof r->reason) != NULL && r->sum == bc_sum(r);
}

void bc_clear(bc_record_t *r) { memset(r, 0, sizeof *r); }

size_t bc_summary(char *out, size_t n, const char *reset, bc_reset_kind_t kind, const bc_record_t *r)
{
    if (!n) return 0;
    const bool have = r && kind != BC_RESET_POWERON && bc_valid(r);
    int len;
    if (kind == BC_RESET_DELIBERATE)
        len = have ? snprintf(out, n, "%s: %s", reset, r->reason)
                   : snprintf(out, n, "%s, no reason recorded", reset);
    else if (have)
        len = snprintf(out, n, "%s, last deliberate restart: %s pending%s", reset, r->reason,
                       r->stage == BC_STAGE_SCHEDULED ? " (scheduled)" : "");
    else
        len = snprintf(out, n, "%s", reset);
    if (len < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)len < n ? (size_t)len : n - 1;
}
