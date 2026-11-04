/* Pure: the maintenance UI's glue in surfaces (maintmap.h). */
#include "maintmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The enums deckui mirrors (it may not include netmgr or slotsync): the same values, so a cast is right. */
_Static_assert((int)DUI_WIFI_NONE == (int)NETMGR_STA_NONE && (int)DUI_WIFI_MAIN == (int)NETMGR_STA_MAIN &&
                   (int)DUI_WIFI_BACKUP == (int)NETMGR_STA_BACKUP,
               "deckui's Wi-Fi networks are netmgr's");
_Static_assert((int)DUI_SLOT_IN_SYNC == (int)SLOTSYNC_IN_SYNC && (int)DUI_SLOT_SYNCING == (int)SLOTSYNC_SYNCING &&
                   (int)DUI_SLOT_DIFFERS == (int)SLOTSYNC_DIFFERS &&
                   (int)DUI_SLOT_OTHER_NEWER == (int)SLOTSYNC_OTHER_NEWER &&
                   (int)DUI_SLOT_EMPTY == (int)SLOTSYNC_EMPTY && (int)DUI_SLOT_INVALID == (int)SLOTSYNC_INVALID &&
                   (int)DUI_SLOT_WAITING == (int)SLOTSYNC_WAITING,
               "deckui's slot states are slotsync's");

#define TAN_22_5 0.414213562373095 /* a swipe within 22.5 deg of the horizontal pages (as inputmap.c) */

/* src into dst (n bytes with the NUL), cut to fit. */
static void put(char *dst, size_t n, const char *src)
{
    size_t l = 0;
    while (src && l + 1 < n && src[l]) {
        dst[l] = src[l];
        l++;
    }
    dst[l] = '\0';
}

/* Four dot-separated numbers 0-255. */
static bool is_ipv4(const char *s)
{
    int parts = 0;
    while (s && *s) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) {
            v = v * 10 + (*s++ - '0');
            digits++;
        }
        if (!digits || v > 255) return false;
        parts++;
        if (!*s) break;
        if (*s++ != '.' || parts == 4) return false;
    }
    return parts == 4;
}

static int sat_state(const surf_maint_in_t *in)
{
    if (in->ns->route == NET_ROUTE_NONE) return DUI_SAT_NONET;
    if (in->sel == SEL_AMBIGUOUS) return DUI_SAT_CHOOSE;
    if (in->too_old) return DUI_SAT_TOO_OLD;
    if (in->live) return DUI_SAT_CONNECTED;
    if (in->target_host && in->target_host[0]) return DUI_SAT_RECON;
    return DUI_SAT_FINDING;
}

void surf_maint_box(dui_box_t *b, const surf_maint_in_t *in)
{
    memset(b, 0, sizeof *b); /* padding too: callers compare snapshots with memcmp */
    const netmgr_status_t *ns = in->ns;
    b->mode = in->dock ? DUI_MODE_DOCK : DUI_MODE_SAT;

    b->sat.state = (uint8_t)sat_state(in);
    b->sat.auto_disc = in->companion_auto;
    for (int i = 0; i < in->ncomps && b->sat.ncomps < DUI_COMPS_MAX; i++) {
        dui_comp_t *c = &b->sat.comp[b->sat.ncomps++];
        put(c->name, sizeof c->name, in->comps[i].name);
        put(c->ip, sizeof c->ip, in->comps[i].host);
    }
    const char *host = in->target_host ? in->target_host : "";
    const char *name = host;
    for (int i = 0; i < in->ncomps; i++) /* the instance it connects to: its name */
        if (host[0] && !strcmp(in->comps[i].host, host) && in->comps[i].name[0]) name = in->comps[i].name;
    put(b->sat.target.name, sizeof b->sat.target.name, name);
    put(b->sat.target.ip, sizeof b->sat.target.ip, is_ipv4(host) ? host : "");
    put(b->sat.saved_ip, sizeof b->sat.saved_ip, is_ipv4(in->companion_host) ? in->companion_host : "");
    b->sat.port = in->companion_port;
    put(b->sat.version, sizeof b->sat.version, in->companion_version);

    b->dock.port = in->dock_port;
    b->dock.no_free = in->dock && in->dock_port == 0;
    b->deck_limit = in->deck_limit; /* "Max N decks" */
    put(b->dock.peer, sizeof b->dock.peer, in->dock_peer);

    b->net.eth_link = ns->eth_link;
    put(b->net.eth_ip, sizeof b->net.eth_ip, ns->eth_link ? ns->eth_addr : ""); /* its own, while linked */
    b->net.eth_dhcp = in->eth_dhcp;
    const int from = dui_eth_static_pick(in->eth_saved, in->eth_lease, NULL);
    b->net.eth_static_ok = from != DUI_ETH_FROM_NONE;
    b->net.eth_static_saved = from == DUI_ETH_FROM_SAVED;
    put(b->net.wifi_main, sizeof b->net.wifi_main, in->wifi_main);
    put(b->net.wifi_backup, sizeof b->net.wifi_backup, in->wifi_backup);
    b->net.wifi_off = !ns->wifi_enabled;
    b->net.wifi_net = ns->sta_net;
    b->net.rssi = ns->sta_rssi;
    b->net.wifi_trying = ns->sta_trying;
    b->net.wifi_not_found = ns->sta_not_found;
    b->net.radio_down = ns->radio_down;
    put(b->net.wifi_ip, sizeof b->net.wifi_ip, ns->sta_addr); /* its own while joined, whatever routes */
    b->net.ap_on = ns->ap_on && in->ap_shown;
    put(b->net.ap_ssid, sizeof b->net.ap_ssid, in->ap_ssid && in->ap_ssid[0] ? in->ap_ssid : ns->ap_ssid);
    put(b->net.ap_pass, sizeof b->net.ap_pass, in->ap_open ? "" : in->ap_pass);
    b->net.ap_secured = !in->ap_open;
    put(b->net.ap_ip, sizeof b->net.ap_ip, ns->ap_ip);
    put(b->net.name, sizeof b->net.name, in->box_name);

    put(b->box.version, sizeof b->box.version, in->version);
    b->box.uptime_s = in->uptime_s;
    b->box.pending = in->pending;
    if (in->ss) {
        b->box.slot = (uint8_t)in->ss->state;
        b->box.pct = (uint8_t)(in->ss->pct < 0 ? 0 : in->ss->pct > 100 ? 100 : in->ss->pct);
        put(b->box.other_version, sizeof b->box.other_version, in->ss->other_version);
        b->box.other_valid = in->ss->other_valid;
        b->box.other_foreign = in->ss->other_foreign;
        b->box.flash_busy = in->ss->flash_busy;
        b->box.running_b = !strcmp(in->ss->running, "ota_1");
    } else {
        b->box.slot = DUI_SLOT_WAITING;
    }

    b->brightness = in->brightness;
    b->sound = in->sound;
    b->flash = in->flash;
    b->locked = in->locked;
    b->ondemand = in->ondemand;
    b->connected = in->connected;
    put(b->toast, sizeof b->toast, in->toast);
    b->toast_state = in->toast_state;
}

surf_input_to_t surf_input_route(bool has_display, bool ondemand, bool ui_shown, bool companion_drives,
                                 bool handing_back, bool loss)
{
    if (ui_shown) return SURF_TO_BOX;
    if (ondemand && has_display) return SURF_TO_DROP; /* on its way: never Companion meanwhile */
    if (handing_back) return SURF_TO_DROP;            /* Companion's images not back yet */
    if (companion_drives) return SURF_TO_COMPANION;
    return loss && has_display ? SURF_TO_WAKE : SURF_TO_DROP; /* "Reconnecting": the box's screens, now */
}

static bool od_screen(const deck_model_t *m) { return m && deck_model_has_display(m); }

/* a is bigger than b (the order). */
static bool od_bigger(const surf_od_cand_t *a, const surf_od_cand_t *b)
{
    const int ka = a->model->cols * a->model->rows, kb = b->model->cols * b->model->rows;
    if (ka != kb) return ka > kb;
    if (a->model->key_px != b->model->key_px) return a->model->key_px > b->model->key_px;
    const bool ba = a->model->lcd_w || a->model->info_w, bb = b->model->lcd_w || b->model->info_w;
    if (ba != bb) return ba;
    return a->seq < b->seq;
}

int surf_od_pick(const surf_od_cand_t *c, int n)
{
    int best = -1;
    for (int i = 0; c && i < n; i++)
        if (od_screen(c[i].model) && c[i].device_id && c[i].device_id[0] && (best < 0 || od_bigger(&c[i], &c[best])))
            best = i;
    return best;
}

int surf_od_find(const surf_od_cand_t *c, int n, const char *device_id)
{
    for (int i = 0; c && device_id && device_id[0] && i < n; i++)
        if (od_screen(c[i].model) && c[i].device_id && !strcmp(c[i].device_id, device_id)) return i;
    return -1;
}

static int od_at(const surf_od_set_t *s, const char *device_id)
{
    for (int i = 0; s && device_id && device_id[0] && i < s->n; i++)
        if (!strcmp(s->id[i], device_id)) return i;
    return -1;
}

bool surf_od_here(const surf_od_set_t *s, const char *device_id) { return od_at(s, device_id) >= 0; }

bool surf_od_add(surf_od_set_t *s, const char *device_id)
{
    if (!s || !device_id || !device_id[0] || od_at(s, device_id) >= 0 || s->n >= SURF_MAX_DECKS) return false;
    snprintf(s->id[s->n++], sizeof s->id[0], "%s", device_id);
    return true;
}

bool surf_od_del(surf_od_set_t *s, const char *device_id)
{
    const int i = od_at(s, device_id);
    if (i < 0) return false;
    memmove(s->id[i], s->id[i + 1], (size_t)(s->n - i - 1) * sizeof s->id[0]);
    s->n--;
    return true;
}

int surf_od_prune(surf_od_set_t *s, const surf_od_cand_t *c, int n, char gone[][48], int max)
{
    int k = 0;
    for (int i = 0; s && i < s->n;) {
        if (surf_od_find(c, n, s->id[i]) >= 0) {
            i++;
            continue;
        }
        if (gone && k < max) snprintf(gone[k], 48, "%s", s->id[i]);
        k++;
        surf_od_del(s, s->id[i]);
    }
    return k;
}

static int held_add(const char **out, int k, int max, const char *id)
{
    if (!id || !id[0] || k >= max) return k;
    for (int i = 0; i < k; i++)
        if (!strcmp(out[i], id)) return k;
    out[k] = id;
    return k + 1;
}

int surf_held_decks(const surf_od_set_t *s, const surf_hold_cand_t *c, int n, const char **out, int max)
{
    int k = 0;
    for (int i = 0; s && i < s->n; i++) k = held_add(out, k, max, s->id[i]);
    for (int i = 0; c && i < n; i++)
        if (c[i].covered || c[i].handing_back) k = held_add(out, k, max, c[i].device_id);
    return k;
}

int surf_bright_apply(uint8_t deck_brightness, int applied)
{
    return deck_brightness && deck_brightness != applied ? deck_brightness : -1;
}

surf_bright_path_t surf_bright_box_path(bool dock, bool owned)
{
    return dock && owned ? SURF_BRIGHT_DOCKCACHE : SURF_BRIGHT_DIRECT;
}

surf_bright_path_t surf_bright_comp_path(bool covered) { return covered ? SURF_BRIGHT_KEEP : SURF_BRIGHT_DIRECT; }

int surf_bright_handback(int companion_last, int applied) { return applied >= 0 && companion_last >= 0 ? companion_last : -1; }


surf_handback_t surf_handback_step(bool handed, bool drained, int64_t now_us, int64_t until_us)
{
    if (handed && drained) return SURF_HB_BACK; /* on the deck, not merely queued for it */
    return now_us >= until_us ? SURF_HB_LATE : SURF_HB_WAIT;
}

surf_nav_ev_t surf_nav_event(const deck_model_t *m, const deck_event_t *ev)
{
    surf_nav_ev_t n = {0};
    if (!m || !ev || !m->cols) return n;
    const int nkeys = m->cols * m->rows;
    switch (ev->kind) {
    case DECK_EV_KEY:
        if (ev->index < nkeys) {
            const int c = ev->index % m->cols;
            n.kind = SURF_NAV_KEY;
            n.row = ev->index / m->cols;
            n.col = m->rtl ? m->cols - 1 - c : c;
            n.down = ev->pressed;
        } else if (m->touch_keys && ev->index < nkeys + 2 && ev->pressed) {
            n.kind = SURF_NAV_TOUCH;
            n.steps = ev->index == nkeys ? -1 : 1;
        }
        break;
    case DECK_EV_ENC_ROTATE:
        if (ev->delta) {
            n.kind = SURF_NAV_DIAL;
            n.steps = ev->delta;
        }
        break;
    case DECK_EV_ENC_PRESS:
        if (ev->pressed) n.kind = SURF_NAV_BACK;
        break;
    case DECK_EV_TOUCH_SWIPE: {
        const long dx = (long)ev->x2 - ev->x, dy = (long)ev->y2 - ev->y;
        if (dx && (double)labs(dy) <= TAN_22_5 * (double)labs(dx)) {
            n.kind = SURF_NAV_DIAL;
            n.steps = dx < 0 ? 1 : -1; /* swipe left: the next page */
        }
        break;
    }
    case DECK_EV_TOUCH_TAP: /* the + / + XL strip: its indicators; the Neo's info bar isn't touch */
        if (m->lcd_w) {
            n.kind = SURF_NAV_BAR;
            n.x = ev->x;
        }
        break;
    default: break;
    }
    return n;
}
