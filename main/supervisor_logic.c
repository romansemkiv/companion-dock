#include "supervisor_logic.h"

#include <stddef.h>
#include <string.h>

/* ---- C6 lost ---- */

void c6w_init(c6w_t *w) { memset(w, 0, sizeof *w); }

bool c6w_step(c6w_t *w, const c6w_in_t *in, int64_t now_us, uint32_t restarts)
{
    if (in->eth_up) {
        w->counting = false;
        return false;
    }
    if (!w->counting) {
        w->counting = true;
        w->eth_down_since_us = now_us;
    }
    if (in->c6_ready || !in->radio_wanted) return false;
    if (now_us - w->eth_down_since_us < C6W_ETH_DOWN_US) return false;
    return restarts < C6W_FREE_RESTARTS || now_us >= C6W_SPACING_US;
}

/* "C6LR", with the record's size in the top byte (another layout never reads as this one). */
#define C6W_MAGIC (0x43364c52u ^ ((uint32_t)sizeof(c6w_rec_t) << 24))

/* FNV-1a over everything before the checksum. */
static uint32_t rec_sum(const c6w_rec_t *r)
{
    const unsigned char *p = (const unsigned char *)r;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(c6w_rec_t, sum); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

uint32_t c6w_rec_count(const c6w_rec_t *r)
{
    return r->magic == C6W_MAGIC && r->sum == rec_sum(r) ? r->count : 0;
}

void c6w_rec_set(c6w_rec_t *r, uint32_t count)
{
    memset(r, 0, sizeof *r);
    r->magic = C6W_MAGIC;
    r->count = count;
    r->sum = rec_sum(r);
}

/* ---- OTA self-check ---- */

bool ota_net_ok(const ota_net_t *n, const char **why)
{
    const char *w = "";
    bool ok;
    switch (n->via) {
    case NETMGR_IF_ETH:
        ok = n->eth_ip;
        if (!ok) w = "the update came over Ethernet, which has no IPv4 address";
        break;
    case NETMGR_IF_STA:
        ok = n->sta_ip;
        if (!ok) w = "the update came over Wi-Fi, whose station has no IPv4 address";
        break;
    case NETMGR_IF_AP:
        ok = n->ap_on;
        if (!ok) w = "the update came over the hotspot, which is not running";
        break;
    default:
        ok = n->routed || n->ap_on;
        if (!ok) w = "no network route and no hotspot";
        break;
    }
    if (why) *why = w;
    return ok;
}

ota_verdict_t ota_verdict(bool healthy, int64_t since_start_us)
{
    if (healthy) return OTA_CONFIRM;
    return since_start_us >= OTA_GIVE_UP_US ? OTA_ROLLBACK : OTA_WAIT;
}

bool ota_force_hotspot(bool pending, netmgr_if_t via, bool ap_enabled, bool already_forced)
{
    return pending && via == NETMGR_IF_AP && !ap_enabled && !already_forced;
}

/* ---- the Companion pin's save ---- */

bool pin_save_due(bool changed, int64_t now_us, int64_t last_save_us)
{
    return changed && (last_save_us < 0 || now_us - last_save_us >= PIN_SAVE_MIN_US);
}
