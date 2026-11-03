/* Pure: the deck's static-address editor (ipedit.h), and its host mode for the Companion's IP. */
#include "deckui/ipedit.h"

#include <stdio.h>
#include <string.h>

#include "deckui/nav.h"
#include "deckui/pin.h"
#include "dui_int.h"
#include "raster.h"

static const dui__pad_ids_t IDS = {.digit0 = DUI_IPE_A_DIGIT, .del = DUI_IPE_A_DEL, .cancel = DUI_IPE_A_CANCEL,
                                   .ok = DUI_IPE_A_NEXT, .down = DUI_IPE_A_DOWN, .up = DUI_IPE_A_UP, .ok_text = "Next"};

static void clear(dui_ipe_out_t *o) { memset(o, 0, sizeof *o); }

static void no_held(dui_ipe_t *e)
{
    memset(&e->held, 0, sizeof e->held);
    e->held.col = e->held.row = -1;
}

static bool host(const dui_ipe_t *e) { return e->kind == DUI_IPE_HOST; }

/* the page dir (+1 / -1) away, wrapping over the pages of the layout made last (a 3-page summary too) */
static uint8_t step_page(const dui_ipe_t *e, int dir)
{
    const int n = e->pages ? e->pages : 1;
    return (uint8_t)(((e->pg % n) + n + (dir > 0 ? 1 : -1)) % n);
}
/* Save's hold: Ethernet's, the Companion IP's */
static int64_t save_ms(const dui_ipe_t *e) { return host(e) ? DUI_HOLD_COMP_MS : DUI_HOLD_ETH_MS; }

/* ---- the mask ---- */

void dui_mask_of_prefix(int prefix, uint8_t out[4])
{
    if (prefix < 0) prefix = 0;
    if (prefix > 32) prefix = 32;
    const uint32_t m = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(m >> (24 - 8 * i));
}

int dui_prefix_of_mask(const uint8_t m[4])
{
    const uint32_t v = (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
    int n = 0;
    while (n < 32 && (v & (0x80000000u >> n))) n++;
    const uint32_t want = n ? 0xFFFFFFFFu << (32 - n) : 0;
    return v == want ? n : -1;
}

static uint32_t u32(const uint8_t q[4]) { return (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]; }

/* ---- the fields ---- */

static int quad_of(int field) { return field == DUI_IPE_IP ? 0 : field == DUI_IPE_GW ? 1 : field == DUI_IPE_DNS ? 2 : -1; }
static bool full(const dui_quad_t *q) { return q->set == 0x0F; }
static bool empty(const dui_quad_t *q) { return q->set == 0; }
static bool has(const dui_quad_t *q, int i) { return q->set & (1u << i); }
static int ndig(int v) { return v < 10 ? 1 : v < 100 ? 2 : 3; }

static void quad_from(dui_quad_t *q, const char *s, bool zero_empty)
{
    memset(q, 0, sizeof *q);
    uint8_t v[4];
    if (!dui_ipv4_parse(s, v)) return;
    if (zero_empty && !v[0] && !v[1] && !v[2] && !v[3]) return;
    memcpy(q->v, v, 4);
    q->set = 0x0F;
}

static void quad_str(const dui_quad_t *q, char out[16])
{
    snprintf(out, 16, "%u.%u.%u.%u", q->v[0], q->v[1], q->v[2], q->v[3]);
}

int dui_ipe_check(const dui_ipe_t *e, int field)
{
    const dui_quad_t *ip = &e->q[0], *gw = &e->q[1], *dns = &e->q[2];
    uint8_t m[4];
    dui_mask_of_prefix(e->prefix, m);
    const uint32_t mask = u32(m), a = u32(ip->v);
    const bool host = ip->v[0] != 0 && ip->v[0] != 127 && ip->v[0] < 224; /* not 0/8, 127/8, 224/3 */
    switch (field) {
    case DUI_IPE_IP: return !full(ip) ? DUI_IPE_E_IP_PART : !host ? DUI_IPE_E_RESERVED : DUI_IPE_OK;
    case DUI_IPE_MASK:
        if (!full(ip)) return DUI_IPE_E_IP_PART;
        if (!host) return DUI_IPE_E_RESERVED;
        if ((a & mask) == a) return DUI_IPE_E_NETWORK;
        if ((a | ~mask) == a) return DUI_IPE_E_BROADCAST;
        return DUI_IPE_OK;
    case DUI_IPE_GW: {
        if (empty(gw)) return DUI_IPE_OK;
        if (!full(gw)) return DUI_IPE_E_GW_PART;
        const uint32_t g = u32(gw->v);
        if ((g & mask) != (a & mask)) return DUI_IPE_E_GW_SUBNET;
        return g == a || (g & mask) == g || (g | ~mask) == g ? DUI_IPE_E_GW_HOST : DUI_IPE_OK;
    }
    case DUI_IPE_DNS: return empty(dns) || full(dns) ? DUI_IPE_OK : DUI_IPE_E_DNS_PART;
    default: return DUI_IPE_OK;
    }
}

void dui_ipe_values(const dui_ipe_t *e, dui_ipset_t *out)
{
    memset(out, 0, sizeof *out);
    uint8_t m[4];
    dui_mask_of_prefix(e->prefix, m);
    quad_str(&e->q[0], out->ip);
    snprintf(out->mask, sizeof out->mask, "%u.%u.%u.%u", m[0], m[1], m[2], m[3]);
    if (full(&e->q[1])) quad_str(&e->q[1], out->gw);
    else snprintf(out->gw, sizeof out->gw, "0.0.0.0"); /* none: Eth.SetConfig takes a quad */
    if (full(&e->q[2])) quad_str(&e->q[2], out->dns);
}

/* ---- the entry ---- */

/* Octet oct of the field now: nothing typed there yet; adv: reached by an auto-advance. */
static void enter_octet(dui_ipe_t *e, int oct, bool adv)
{
    e->oct = (uint8_t)oct;
    e->fresh = true;
    e->nd = 0;
    e->adv = adv;
}

static void to_field(dui_ipe_t *e, int field, int oct, bool fresh)
{
    e->field = (uint8_t)field;
    e->pg = 0;
    enter_octet(e, oct, false);
    e->fresh = fresh;
    const int qi = quad_of(field);
    if (!fresh && qi >= 0 && has(&e->q[qi], oct)) e->nd = (uint8_t)ndig(e->q[qi].v[oct]); /* back on it, as typed */
}

bool dui_ipe_digit(dui_ipe_t *e, int digit)
{
    const int qi = quad_of(e->field);
    if (qi < 0 || digit < 0 || digit > 9) return false;
    dui_quad_t *q = &e->q[qi];
    const int o = e->oct;
    if (!has(q, o) || e->fresh) { /* the octet's first digit: it replaces what was there (a prefill too) */
        q->v[o] = (uint8_t)digit;
        q->set |= (uint8_t)(1u << o);
        e->fresh = e->adv = false;
        e->nd = 1;
        return true;
    }
    const int nv = q->v[o] * 10 + digit;
    if (nv > 255) { /* the 255 rule: the digit starts the next octet */
        if (o >= 3) return false;
        enter_octet(e, o + 1, false); /* typed into: not an empty advance */
        return dui_ipe_digit(e, digit);
    }
    if (e->nd >= 3) return false; /* the last octet full */
    q->v[o] = (uint8_t)nv;
    e->adv = false;
    if (++e->nd == 3 && o < 3) enter_octet(e, o + 1, true); /* 3 digits: on; Next there confirms */
    return true;
}

/* The last digit of octet o off (the octet empty after its only one). */
static void drop_digit(dui_quad_t *q, int o)
{
    if (!has(q, o)) return;
    if (q->v[o] < 10) {
        q->v[o] = 0;
        q->set &= (uint8_t)~(1u << o);
    } else {
        q->v[o] = (uint8_t)(q->v[o] / 10);
    }
}

/* Back into octet o (fresh no more), its last digit off. */
static void back_into(dui_ipe_t *e, dui_quad_t *q, int o)
{
    e->oct = (uint8_t)o;
    e->fresh = e->adv = false;
    e->nd = has(q, o) ? (uint8_t)ndig(q->v[o]) : 0;
    drop_digit(q, o);
    if (e->nd) e->nd--;
}

void dui_ipe_del(dui_ipe_t *e)
{
    if (e->field == DUI_IPE_SUM) { /* back to the DNS: its last octet, its first when it is empty */
        if (host(e)) to_field(e, DUI_IPE_IP, 3, false); /* host mode: back to the IP's last octet */
        else to_field(e, DUI_IPE_DNS, empty(&e->q[2]) ? 0 : 3, false);
        return;
    }
    if (e->field == DUI_IPE_MASK) { /* back to the IP's last octet */
        to_field(e, DUI_IPE_IP, 3, false);
        return;
    }
    dui_quad_t *q = &e->q[quad_of(e->field)];
    if (e->adv && e->oct > 0) { /* right after an auto-advance: the digit just typed, in the octet before */
        back_into(e, q, e->oct - 1);
        return;
    }
    if (has(q, e->oct)) {
        back_into(e, q, e->oct);
        return;
    }
    if (e->oct > 0) { /* an empty octet: back one, its last digit off */
        back_into(e, q, e->oct - 1);
        return;
    }
    if (e->field == DUI_IPE_GW) to_field(e, DUI_IPE_MASK, 0, true);
    else if (e->field == DUI_IPE_DNS) to_field(e, DUI_IPE_GW, empty(&e->q[1]) ? 0 : 3, false);
}

int dui_ipe_next(dui_ipe_t *e)
{
    if (e->field == DUI_IPE_SUM) return DUI_IPE_OK;
    const int qi = quad_of(e->field);
    if (qi >= 0 && e->adv) { /* right after an auto-advance: it confirms the move, never skips */
        e->adv = false;
        return DUI_IPE_OK;
    }
    if (qi >= 0 && e->oct < 3 && !(qi > 0 && empty(&e->q[qi]))) { /* the next octet; an empty Gateway / DNS skips */
        enter_octet(e, e->oct + 1, false);
        return DUI_IPE_OK;
    }
    const int err = dui_ipe_check(e, e->field);
    if (err) return err;
    to_field(e, host(e) ? DUI_IPE_SUM : e->field + 1, 0, true); /* host mode: the IP, then the summary */
    return DUI_IPE_OK;
}

void dui_ipe_step(dui_ipe_t *e, int steps)
{
    if (e->field == DUI_IPE_MASK) {
        int p = e->prefix + steps;
        e->prefix = (uint8_t)(p < 1 ? 1 : p > 30 ? 30 : p);
        return;
    }
    const int qi = quad_of(e->field);
    if (qi < 0) return;
    dui_quad_t *q = &e->q[qi];
    const int v = has(q, e->oct) ? q->v[e->oct] : (steps > 0 ? -1 : 256); /* empty: from 0 up, from 255 down */
    q->v[e->oct] = (uint8_t)(((v + steps) % 256 + 256) % 256);
    q->set |= (uint8_t)(1u << e->oct);
    e->fresh = e->adv = false;
    e->nd = (uint8_t)ndig(q->v[e->oct]);
}

/* ---- open, close ---- */

void dui_ipe_open(dui_ipe_t *e, const dui_deck_t *d, const dui_ipset_t *pre, int64_t now, dui_ipe_out_t *o)
{
    clear(o);
    memset(e, 0, sizeof *e);
    no_held(e);
    e->open = true;
    e->style = (uint8_t)dui_pin_style(d);
    e->prefix = 24;
    if (pre) {
        quad_from(&e->q[0], pre->ip, true);
        uint8_t m[4];
        const int p = dui_ipv4_parse(pre->mask, m) ? dui_prefix_of_mask(m) : -1;
        if (p >= 1 && p <= 30) e->prefix = (uint8_t)p;
        quad_from(&e->q[1], pre->gw, true);
        quad_from(&e->q[2], pre->dns, true);
    }
    to_field(e, DUI_IPE_IP, 0, true);
    e->last_press = now;
    o->redraw = true;
}

void dui_ipe_open_host(dui_ipe_t *e, const dui_deck_t *d, const char *ip, uint16_t port, int64_t now, dui_ipe_out_t *o)
{
    dui_ipe_open(e, d, NULL, now, o);
    e->kind = DUI_IPE_HOST;
    e->port = port;
    if (ip) quad_from(&e->q[0], ip, true);
}

void dui_ipe_close(dui_ipe_t *e)
{
    e->open = false;
    no_held(e);
}

static void closed(dui_ipe_t *e, dui_ipe_out_t *o)
{
    o->last_press = e->last_press;
    dui_ipe_close(e);
    o->closed = o->redraw = true;
}

/* ---- the words ---- */

static const struct {
    const char *full, *two[2], *one;
    const char *three[3];
} ERR[] = {
    [DUI_IPE_E_IP_PART] = {"Fill all 4 octets", {"Fill all", "4 octets"}, "Fill all 4 octets", {"Fill", "all 4", "octets"}},
    [DUI_IPE_E_NETWORK] = {"IP is the network address", {"Network", "address"}, "Network address", {"IP is the", "network", "address"}},
    [DUI_IPE_E_BROADCAST] = {"IP is the broadcast address", {"Broadcast", "address"}, "Broadcast address",
                             {"IP is the", "broadcast", "address"}},
    [DUI_IPE_E_GW_PART] = {"Fill all 4 octets", {"Fill all", "4 octets"}, "Fill all 4 octets", {"Fill", "all 4", "octets"}},
    [DUI_IPE_E_GW_SUBNET] = {"Gateway not in subnet", {"Gateway", "not in subnet"}, "Not in subnet", {"Gateway", "not in", "subnet"}},
    [DUI_IPE_E_DNS_PART] = {"Fill all 4 octets", {"Fill all", "4 octets"}, "Fill all 4 octets", {"Fill", "all 4", "octets"}},
    [DUI_IPE_E_RESERVED] = {"Not a host address", {"Not a host", "address"}, "Not a host", {"Not a", "host", "address"}},
    [DUI_IPE_E_GW_HOST] = {"Gateway can't be that", {"Gateway", "not a host"}, "Not a host", {"Gateway", "not a", "host"}},
};

static int err_now(const dui_ipe_t *e, int64_t now) { return e->err && now < e->err_until ? e->err : 0; }

static const char *field_name(const dui_ipe_t *e, int field)
{
    switch (field) {
    case DUI_IPE_IP: return host(e) ? "Companion IP" : "IP address";
    case DUI_IPE_MASK: return "Mask";
    case DUI_IPE_GW: return "Gateway";
    case DUI_IPE_DNS: return "DNS";
    default: return "Save?";
    }
}

/* The field's one word on a key ("IP", "Mask", "Gateway", "DNS"). */
static const char *field_word(const dui_ipe_t *e, int field)
{
    return field == DUI_IPE_IP ? "IP" : field_name(e, field);
}

int dui_ipe_message(const dui_ipe_t *e, int64_t now, char *out, int n)
{
    if (n <= 0) return 0;
    const int er = err_now(e, now);
    if (er) return snprintf(out, (size_t)n, "%s", ERR[er].full);
    if (e->field == DUI_IPE_MASK) return snprintf(out, (size_t)n, "Mask /%u", e->prefix);
    return snprintf(out, (size_t)n, "%s", field_name(e, e->field));
}

/* ---- the layout ---- */

#define put dui__lay_put

static dui_tile_t *key(dui_layout_t *L, int page, int col, int row, uint16_t action)
{
    return dui__pad_key(L, page, col, row, action, &IDS);
}

static int tint_now(const dui_ipe_t *e, int64_t now) { return err_now(e, now) ? DUI_TINT_BAD : DUI_TINT_NEUTRAL; }

static void fill(const dui_ipe_t *e, dui_layout_t *L, int col, int row, int64_t now)
{
    put(L, 0, col, row, DUI_TILE_FILL)->tint = (uint8_t)tint_now(e, now);
}

static dui_tile_t *word(const dui_ipe_t *e, dui_layout_t *L, int col, int row, const char *text, int64_t now)
{
    dui_tile_t *t = put(L, 0, col, row, DUI_TILE_WORD);
    t->tint = (uint8_t)tint_now(e, now);
    t->role = (uint8_t)(err_now(e, now) ? DUI_ROLE_BAD : DUI_ROLE_LABEL);
    t->text = text;
    return t;
}

/* The value shown: the field's octets (the mask's when on the Mask); empty octets "". */
static void shown_octets(const dui_ipe_t *e, uint8_t v[4], uint8_t *set)
{
    if (e->field == DUI_IPE_MASK) {
        dui_mask_of_prefix(e->prefix, v);
        *set = 0x0F;
        return;
    }
    const dui_quad_t *q = &e->q[quad_of(e->field)];
    memcpy(v, q->v, 4);
    *set = q->set;
}

/* Octets first..first + n as one key's text in e->pool (from *used): "192.168." with a trailing dot when
 * more follow on another key. */
static const char *octets_text(dui_ipe_t *e, size_t *used, int first, int n, bool trail)
{
    uint8_t v[4], set;
    shown_octets(e, v, &set);
    char buf[20];
    int k = 0;
    for (int i = first; i < first + n; i++) {
        if (set & (1u << i)) k += snprintf(buf + k, sizeof buf - (size_t)k, "%u", v[i]);
        if (i < first + n - 1 || trail) buf[k++] = '.';
        buf[k] = '\0';
    }
    if (*used + (size_t)k + 1 > sizeof e->pool) return "";
    char *s = e->pool + *used;
    memcpy(s, buf, (size_t)k + 1);
    *used += (size_t)k + 1;
    return s;
}

static dui_tile_t *octets(dui_ipe_t *e, dui_layout_t *L, size_t *used, int col, int row, int first, int n, bool trail,
                          const char *caption, int64_t now)
{
    dui_tile_t *t = put(L, 0, col, row, DUI_TILE_OCTETS);
    t->tint = (uint8_t)tint_now(e, now);
    t->role = DUI_ROLE_ACCENT;
    t->text = octets_text(e, used, first, n, trail);
    t->num = (uint8_t)n;
    t->cur = e->field != DUI_IPE_MASK && e->oct >= first && e->oct < first + n ? (uint8_t)(e->oct - first) : 0xFF;
    t->caption = caption;
    return t;
}

/* The phone pad at (x0, y0) with ⌫ / 0 / Next beside it; on the Mask: − and + in its middle row. */
/* The digits as a phone's, ⌫ / 0 / Next in the column beside them; on the Mask, − and + with the prefix between them
 * ("/24" where the steps change it, not across the deck). */
static void phone_pad(dui_ipe_t *e, dui_layout_t *L, int x0, int y0, size_t *used, int64_t now)
{
    if (e->field == DUI_IPE_MASK) {
        key(L, 0, x0, y0 + 1, DUI_IPE_A_DOWN);
        dui_tile_t *t = put(L, 0, x0 + 1, y0 + 1, DUI_TILE_CAP);
        t->tint = (uint8_t)tint_now(e, now);
        t->role = DUI_ROLE_VALUE;
        t->caption = "Prefix";
        const int k = snprintf(e->pool + *used, sizeof e->pool - *used, "/%u", e->prefix);
        t->text = e->pool + *used;
        *used += (size_t)k + 1;
        key(L, 0, x0 + 2, y0 + 1, DUI_IPE_A_UP);
    } else {
        for (int k = 1; k <= 9; k++) key(L, 0, x0 + (k - 1) % 3, y0 + (k - 1) / 3, (uint16_t)(DUI_IPE_A_DIGIT + k));
        key(L, 0, x0 + 3, y0 + 1, DUI_IPE_A_DIGIT + 0);
    }
    key(L, 0, x0 + 3, y0, DUI_IPE_A_DEL);
    key(L, 0, x0 + 3, y0 + 2, DUI_IPE_A_NEXT);
}

/* The summary: a sub-screen dui_plan lays out on every model. */
static void build_summary(dui_ipe_t *e)
{
    dui_screen_t *s = &e->sum;
    dui_screen_init(s);
    s->status.state = DUI_STATE_NONE;
    s->status.net = e->net;
    s->status.rssi = e->rssi;
    s->status.hotspot = e->hotspot;
    s->status.wifi = e->wifi;
    s->status.eth_link = e->eth_link;
    dui_title(s, "Static IP");
    dui_ipset_t v;
    dui_ipe_values(e, &v);
    char mcap[16];
    snprintf(mcap, sizeof mcap, "Mask /%u", e->prefix);
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_ipv4(s, "IP", v.ip);
    dui_group(s, DUI_ZONE_MAIN, DUI_G_PACK); /* beside the IP when the row holds both (Module 32, + XL) */
    dui_ipv4(s, mcap, v.mask);
    dui_group(s, DUI_ZONE_MAIN, 0);
    if (full(&e->q[1])) dui_ipv4(s, "Gateway", v.gw);
    else dui_cap(s, "Gateway", "None", DUI_ROLE_LABEL);
    dui_group(s, DUI_ZONE_MAIN, DUI_G_PACK);
    if (v.dns[0]) dui_ipv4(s, "DNS", v.dns);
    else dui_cap(s, "DNS", "None", DUI_ROLE_LABEL);
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_tile_t *t = dui_button(s, DUI_ICON_ETH, "Save", DUI_IPE_A_SAVE);
    t->hold_ms = DUI_HOLD_ETH_MS;
    t->hint = dui_str(s, "net resets"); /* "Hold 1.5 s" / "net resets" */
    if (e->locked) t->flags |= DUI_F_LOCKED;
    dui_button(s, DUI_ICON_CLOSE, "Cancel", DUI_IPE_A_CANCEL);
}

/* Host mode's summary: the IP, the port it will use (read-only: the web page sets it), Save and Cancel. */
static void build_host_summary(dui_ipe_t *e)
{
    dui_screen_t *s = &e->sum;
    dui_screen_init(s);
    s->status.state = DUI_STATE_NONE;
    s->status.net = e->net;
    s->status.rssi = e->rssi;
    s->status.hotspot = e->hotspot;
    s->status.wifi = e->wifi;
    s->status.eth_link = e->eth_link;
    dui_title(s, "Companion IP");
    char ip[16], port[8];
    quad_str(&e->q[0], ip);
    snprintf(port, sizeof port, "%u", (unsigned)(e->port ? e->port : DUI_COMP_PORT));
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_ipv4(s, "IP", ip);
    dui_group(s, DUI_ZONE_MAIN, DUI_G_PACK); /* beside the IP when the row holds both */
    dui_port(s, "Port", port);
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_tile_t *t = dui_button(s, DUI_ICON_COMPANION, "Save", DUI_IPE_A_SAVE);
    t->hold_ms = DUI_HOLD_COMP_MS;
    t->hint = dui_str(s, "reconnects"); /* "Hold 1.5 s" / "reconnects" */
    if (e->locked) t->flags |= DUI_F_LOCKED;
    dui_button(s, DUI_ICON_CLOSE, "Cancel", DUI_IPE_A_CANCEL);
}

void dui_ipe_layout(const dui_deck_t *d, dui_ipe_t *e, int64_t now, dui_layout_t *L)
{
    if (e->field == DUI_IPE_SUM) {
        if (host(e)) build_host_summary(e);
        else build_summary(e);
        dui_plan(d, &e->sum, L);
        e->pages = (uint8_t)L->pages;
        return;
    }
    dui__lay_init(L, d);
    size_t used = 0;
    const int er = err_now(e, now);
    const bool mask = e->field == DUI_IPE_MASK;
    char cap[16];
    snprintf(cap, sizeof cap, "%s", mask ? "" : field_word(e, e->field));
    switch (e->style) {
    case DUI_PIN_PHONE: {
        key(L, 0, 0, 0, DUI_IPE_A_CANCEL)->tint = (uint8_t)tint_now(e, now);
        if (er) {
            for (int i = 0; i < 3; i++) word(e, L, 1 + i, 0, ERR[er].three[i], now);
            for (int c = 4; c < d->cols; c++) fill(e, L, c, 0, now);
        } else {
            word(e, L, 1, 0, mask ? "Mask" : host(e) ? "Companion IP" : field_word(e, e->field), now);
            for (int i = 0; i < 4; i++) octets(e, L, &used, 2 + i, 0, i, 1, false, NULL, now);
            for (int c = 6; c < d->cols; c++) fill(e, L, c, 0, now);
        }
        dui__lay_align_row0(L);
        phone_pad(e, L, (d->cols - 3) / 2, d->rows - 3, &used, now);
        break;
    }
    case DUI_PIN_PHONE_BAR: /* the field and its value on the strip */
        key(L, 0, 0, 0, DUI_IPE_A_CANCEL);
        phone_pad(e, L, (d->cols - 3) / 2, d->rows - 3, &used, now);
        break;
    case DUI_PIN_ROWS: {
        const int st = tint_now(e, now);
        key(L, 0, 0, 0, DUI_IPE_A_CANCEL)->tint = (uint8_t)st;
        key(L, 0, 1, 0, DUI_IPE_A_DEL)->tint = (uint8_t)st;
        if (er) {
            word(e, L, 2, 0, ERR[er].two[0], now);
            word(e, L, 3, 0, ERR[er].two[1], now);
            dui__lay_align_row0(L);
        } else { /* the address on 2 keys, 2 octets each, the caption on the first */
            if (mask) snprintf(cap, sizeof cap, "Mask /%u", e->prefix);
            dui_tile_t *a = octets(e, L, &used, 2, 0, 0, 2, true, NULL, now);
            const size_t k = strlen(cap) + 1;
            if (used + k <= sizeof e->pool) {
                memcpy(e->pool + used, cap, k);
                a->caption = e->pool + used;
                used += k;
            }
            dui_tile_t *b = octets(e, L, &used, 3, 0, 2, 2, false, NULL, now);
            const int pa = dui__octets_px(a, d->key_px), pb = dui__octets_px(b, d->key_px);
            a->fit.px = b->fit.px = (uint8_t)(pa < pb ? pa : pb); /* the pair at one size */
        }
        key(L, 0, 4, 0, DUI_IPE_A_NEXT)->tint = (uint8_t)st;
        if (mask) {
            key(L, 0, 1, 1, DUI_IPE_A_DOWN);
            key(L, 0, 3, 1, DUI_IPE_A_UP);
        } else {
            for (int k = 1; k <= 5; k++) key(L, 0, k - 1, 1, (uint16_t)(DUI_IPE_A_DIGIT + k));
            for (int k = 6; k <= 10; k++) key(L, 0, k - 6, 2, (uint16_t)(DUI_IPE_A_DIGIT + k % 10));
        }
        break;
    }
    case DUI_PIN_PAGES:
        if (mask) {
            key(L, 0, 0, 0, DUI_IPE_A_DOWN);
            key(L, 0, 1, 0, DUI_IPE_A_UP);
            key(L, 0, 1, 1, DUI_IPE_A_DEL);
            key(L, 0, 2, 1, DUI_IPE_A_NEXT);
            break;
        }
        L->pages = 2;
        for (int pg = 0; pg < 2; pg++) {
            for (int k = 0; k < 5; k++) { /* 1-5, then 6-0 */
                const int digit = (pg * 5 + k + 1) % 10;
                key(L, pg, k < 4 ? k : 0, k < 4 ? 0 : 1, (uint16_t)(DUI_IPE_A_DIGIT + digit));
            }
            key(L, pg, 1, 1, DUI_IPE_A_DEL);
            key(L, pg, 2, 1, DUI_IPE_A_NEXT);
            dui_tile_t *t = put(L, pg, 3, 1, DUI_TILE_PAGER);
            t->tint = DUI_TINT_MAIN;
            t->action = DUI_IPE_A_PAGE;
        }
        L->pager_col = 3;
        L->pager_row = 1;
        break;
    default: { /* ROLLER: − / the octet / +, then Next, ⌫, ✕ */
        key(L, 0, 0, 0, DUI_IPE_A_DOWN);
        if (er) {
            word(e, L, 1, 0, ERR[er].one, now);
        } else if (mask) {
            dui_tile_t *t = put(L, 0, 1, 0, DUI_TILE_CAP);
            t->tint = DUI_TINT_NEUTRAL;
            t->role = DUI_ROLE_ACCENT;
            t->caption = "Mask";
            const size_t k = (size_t)snprintf(e->pool + used, sizeof e->pool - used, "/%u", e->prefix) + 1;
            t->text = e->pool + used;
            used += k;
        } else {
            snprintf(cap, sizeof cap, "%s %u/4", field_word(e, e->field), e->oct + 1);
            const size_t k = strlen(cap) + 1;
            const char *c = "";
            if (used + k <= sizeof e->pool) {
                memcpy(e->pool + used, cap, k);
                c = e->pool + used;
                used += k;
            }
            octets(e, L, &used, 1, 0, e->oct, 1, false, c, now)->cur = 0;
        }
        key(L, 0, 2, 0, DUI_IPE_A_UP);
        key(L, 0, 0, 1, DUI_IPE_A_NEXT);
        key(L, 0, 1, 1, DUI_IPE_A_DEL);
        key(L, 0, 2, 1, DUI_IPE_A_CANCEL);
        break;
    }
    }
    e->pages = (uint8_t)L->pages; /* the digits' pages (PAGES: 2) */
}

void dui_ipe_decorate(const dui_ipe_t *e, int col, int row, int64_t now, dui_tile_t *t)
{
    if (e->held.col == col && e->held.row == row && !e->held.fired &&
        (t->kind == DUI_TILE_KEY || t->kind == DUI_TILE_PAGER || t->kind == DUI_TILE_BUTTON))
        t->flags |= DUI_F_DOWN;
    if (t->kind == DUI_TILE_BUTTON && t->action == DUI_IPE_A_SAVE) {
        if (e->held.col == col && e->held.row == row && !e->held.fired && !(t->flags & DUI_F_LOCKED)) {
            const int64_t held = now - e->held.at, full_ms = save_ms(e);
            t->hold = (uint8_t)(held >= full_ms ? 255 : held * 255 / full_ms);
        }
        if (now < e->hint_until) t->flags |= DUI_F_HINT;
    }
}

/* ---- input ---- */

static void error(dui_ipe_t *e, int err, int64_t now, dui_ipe_out_t *o)
{
    e->err = (uint8_t)err;
    e->err_until = now + DUI_IPE_ERR_MS;
    o->sound = DUI_SOUND_LOCKED;
}

/* A key's action, on its release (− / + on the press). */
static void run(dui_ipe_t *e, uint16_t a, int64_t now, dui_ipe_out_t *o)
{
    o->redraw = true;
    if (a == DUI_IPE_A_CANCEL) {
        closed(e, o);
        return;
    }
    if (a == DUI_ACT_PAGER || a == DUI_IPE_A_PAGE) {
        e->pg = step_page(e, 1);
        return;
    }
    if (a == DUI_ACT_BACK) {
        dui_ipe_del(e); /* the summary: back to the DNS */
        return;
    }
    if (a == DUI_IPE_A_SAVE) { /* let go before the hold: what it is for */
        e->hint_until = now + DUI_IPE_HINT_MS;
        return;
    }
    e->err = 0; /* a press ends the error */
    if (a >= DUI_IPE_A_DIGIT && a <= DUI_IPE_A_DIGIT + 9) {
        if (!dui_ipe_digit(e, a - DUI_IPE_A_DIGIT)) o->sound = DUI_SOUND_LOCKED;
    } else if (a == DUI_IPE_A_DEL) {
        dui_ipe_del(e);
    } else if (a == DUI_IPE_A_NEXT) {
        const int err = dui_ipe_next(e);
        if (err) error(e, err, now, o);
    }
}

void dui_ipe_key(dui_ipe_t *e, const dui_layout_t *L, int col, int row, bool down, int64_t now, dui_ipe_out_t *o)
{
    clear(o);
    if (!e->open) return;
    if (!down) {
        if (e->held.col != col || e->held.row != row) return; /* not the key that went down */
        e->last_press = now;
        const uint16_t a = e->held.action;
        const bool fired = e->held.fired;
        no_held(e);
        o->redraw = true; /* shown released */
        if (fired || a == DUI_IPE_A_DOWN || a == DUI_IPE_A_UP || !a) return;
        run(e, a, now, o);
        return;
    }
    e->last_press = now;
    if (e->held.col >= 0) return; /* another key is down: one at a time */
    dui_tile_t t;
    const bool has_key = dui_layout_key(L, e->pg, col, row, &t);
    no_held(e);
    e->held.col = (int8_t)col;
    e->held.row = (int8_t)row;
    e->held.at = now;
    e->held.next = now + DUI_IPE_REPEAT_MS;
    const bool pressable = has_key && (t.kind == DUI_TILE_KEY || t.kind == DUI_TILE_PAGER || t.kind == DUI_TILE_BUTTON);
    e->held.action = pressable ? t.action : 0;
    if (!pressable) return;
    o->sound = DUI_SOUND_CLICK;
    o->redraw = true;
    if (t.action == DUI_IPE_A_SAVE && (t.flags & DUI_F_LOCKED)) { /* locked: no hold, the locked sound */
        e->held.fired = true;
        o->sound = DUI_SOUND_LOCKED;
    }
    if (t.action == DUI_IPE_A_DOWN || t.action == DUI_IPE_A_UP) {
        e->err = 0;
        dui_ipe_step(e, t.action == DUI_IPE_A_UP ? 1 : -1);
    }
}

void dui_ipe_dial_turn(dui_ipe_t *e, int steps, int64_t now, dui_ipe_out_t *o)
{
    clear(o);
    if (!e->open || !steps) return;
    e->last_press = now;
    o->redraw = true;
    if (e->field == DUI_IPE_SUM) {
        e->pg = step_page(e, steps);
        return;
    }
    e->err = 0;
    dui_ipe_step(e, steps);
}

void dui_ipe_dial_press(dui_ipe_t *e, int64_t now, dui_ipe_out_t *o)
{
    clear(o);
    if (!e->open) return;
    e->last_press = now;
    if (e->field == DUI_IPE_SUM) return; /* Save is a hold on its key */
    run(e, DUI_IPE_A_NEXT, now, o);
}

void dui_ipe_touch(dui_ipe_t *e, int dir, int64_t now, dui_ipe_out_t *o)
{
    clear(o);
    if (!e->open || !dir) return;
    e->last_press = now;
    e->pg = step_page(e, dir);
    o->redraw = true;
}

void dui_ipe_tick(dui_ipe_t *e, int64_t now, dui_ipe_out_t *o)
{
    clear(o);
    if (!e->open) return;
    if (e->err && now >= e->err_until) {
        e->err = 0;
        o->redraw = true;
    }
    if (e->hint_until && now >= e->hint_until) {
        e->hint_until = 0;
        o->redraw = true;
    }
    if (e->held.col >= 0 && !e->held.fired) {
        const uint16_t a = e->held.action;
        const int64_t held = now - e->held.at;
        if (a == DUI_IPE_A_DEL && held >= DUI_PIN_CANCEL_HOLD_MS) { /* ⌫ held: cancel */
            e->held.fired = true;
            closed(e, o);
            return;
        }
        if (a == DUI_IPE_A_SAVE && held >= save_ms(e) && !e->locked) {
            e->held.fired = true;
            o->save = true;
            o->sound = DUI_SOUND_CONFIRM;
            closed(e, o);
            return;
        }
        if ((a == DUI_IPE_A_DOWN || a == DUI_IPE_A_UP) && now >= e->held.next) { /* repeats, faster later */
            const int k = held >= DUI_IPE_FAST_MS && e->field != DUI_IPE_MASK ? 5 : 1;
            dui_ipe_step(e, a == DUI_IPE_A_UP ? k : -k);
            e->held.next = now + DUI_IPE_EVERY_MS;
            o->redraw = true;
        }
        if (a == DUI_IPE_A_SAVE) o->redraw = true; /* the fill */
    }
    if (e->held.col < 0 && now - e->last_press >= DUI_IPE_IDLE_MS) closed(e, o);
}

void dui_ipe_cancel(dui_ipe_t *e) { no_held(e); }

/* ---- the strip / info bar ---- */

uint32_t dui_ipe_bar_hash(const dui_ipe_t *e, int64_t now)
{
    if (e->field == DUI_IPE_SUM) return dui_bar_hash(&e->sum.status, true, e->pg, 1) ^ (host(e) ? 0x52u : 0x51u);
    uint8_t v[4], set;
    shown_octets(e, v, &set);
    const uint8_t b[] = {e->field, e->oct, set, v[0], v[1], v[2], v[3], e->prefix, (uint8_t)err_now(e, now)};
    uint32_t h = dui__fnv(DUI__FNV_BASIS, b, sizeof b);
    static const uint8_t HOST = 0xC0;
    if (host(e)) h = dui__fnv(h, &HOST, 1); /* "Companion IP", not "IP address": another bar */
    return h ? h : 1;
}

void dui_render_ipe_bar(const dui_ipe_t *e, int64_t now, int w, int h, uint8_t *rgb)
{
    if (e->field == DUI_IPE_SUM) {
        dui_render_bar(&e->sum.status, true, e->pg, 1, w, h, rgb);
        return;
    }
    const dui_canvas_t c = {rgb, w, h};
    const int er = err_now(e, now);
    rs_fill(&c, 0, 0, w, h, er ? dui__tint_rgb(DUI_TINT_BAD) : DUI_RGB_TINT_NEUTRAL);
    const bool small = h < 80;
    const int pad = small ? 8 : 22;
    char m[48];
    const int n = dui_ipe_message(e, now, m, sizeof m);
    const int maxpx = small ? 22 : 40;
    if (er) {
        int fs = dui__font_fit(m, n, w - 2 * pad, maxpx, DUI_TEXT_MIN_PX, 0);
        if (!fs) fs = DUI_TEXT_MIN_PX;
        dui__text(&c, fs, (float)pad, h / 2.0f + 0.4f * fs + 2, m, n, SUI_RGB_BAD, 0, DUI__LEFT);
        return;
    }
    /* the field's name at the left, the value at the right: its octets, the current one outlined */
    int fs = dui__font_fit(m, n, w / 3, small ? 18 : 30, DUI_TEXT_MIN_PX, 0);
    if (!fs) fs = DUI_TEXT_MIN_PX;
    dui__text(&c, fs, (float)pad, h / 2.0f + 0.4f * fs, m, n, SUI_RGB_LABEL, 0, DUI__LEFT);
    uint8_t v[4], set;
    shown_octets(e, v, &set);
    char oc[4][4];
    int ol[4];
    for (int i = 0; i < 4; i++) ol[i] = set & (1u << i) ? snprintf(oc[i], sizeof oc[i], "%u", v[i]) : snprintf(oc[i], sizeof oc[i], "_");
    const int room = w - w / 3 - 3 * pad;
    int px = maxpx;
    int widths[4], dotw = 0, total = 0, gap = 1, ogap = 2;
    for (; px >= DUI_TEXT_MIN_PX; px--) {
        if (!dui__cap(px)) continue;
        gap = px / 12 > 1 ? px / 12 : 1;
        ogap = (int)(px * 0.18f) + 3; /* by the outlined octet: its pad and a little air */
        dotw = dui__text_w(px, ".", 1, 0, NULL);
        total = 3 * dotw + 6 * gap + 2 * (ogap - gap); /* as wide as with an outline in the middle */
        for (int i = 0; i < 4; i++) {
            widths[i] = dui__text_w(px, oc[i], ol[i], 0, NULL);
            const int w00 = dui__text_w(px, "00", 2, 0, NULL);
            if (!(set & (1u << i)) && widths[i] < w00) widths[i] = w00;
            total += widths[i];
        }
        if (total <= room) break;
    }
    if (px < DUI_TEXT_MIN_PX) px = DUI_TEXT_MIN_PX;
    float x = (float)(w - pad - total);
    const float base = h / 2.0f + 0.4f * px, bp = px * 0.18f;
    const int cur = e->field != DUI_IPE_MASK ? e->oct : -1;
    for (int i = 0; i < 4; i++) {
        const bool here = i == cur, none = !(set & (1u << i));
        if (here) {
            const float ch = (float)dui__cap(px);
            rs_draw_rrect(&c, x - bp, base - ch - bp, (float)widths[i] + 2 * bp, ch + 2 * bp, bp, small ? 1.5f : 2.5f,
                          SUI_RGB_ACCENT);
        }
        const int iw = dui__text_w(px, oc[i], ol[i], 0, NULL);
        dui__text(&c, px, x + (float)(widths[i] - iw) / 2, base, oc[i], ol[i],
                  none ? DUI_RGB_DIM : here ? SUI_RGB_VALUE : SUI_RGB_ACCENT, 0, DUI__LEFT);
        x += (float)(widths[i] + (here ? ogap : gap));
        if (i < 3) {
            dui__text(&c, px, x, base, ".", 1, SUI_RGB_LABEL, 0, DUI__LEFT);
            x += (float)(dotw + (i + 1 == cur ? ogap : gap));
        }
    }
}
