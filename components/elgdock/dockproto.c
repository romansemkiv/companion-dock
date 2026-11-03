/* Network Dock payloads, host message classifier and the deck port's routing (dockproto.h). Pure C.
 * Offsets are the ones @elgato-stream-deck/tcp 7.8.0 reads. */
#include "dockproto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HOST_REPORT 0x03 /* first byte of a dock's own report, and of the host's GET_REPORT */
#define CHILD_PUSH0 0x01
#define CHILD_PUSH1 0x0b
#define KEEPALIVE1 0x0a
#define ACK1 0x1a

static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (s && n < max && s[n]) n++;
    return n;
}

static void host_report(uint8_t out[DOCK_REPLY_LEN], uint8_t id)
{
    memset(out, 0, DOCK_REPLY_LEN);
    out[0] = HOST_REPORT;
    out[1] = id;
}

size_t dockproto_devinfo(uint8_t out[DOCK_REPLY_LEN], uint16_t vid, uint16_t pid)
{
    host_report(out, DOCK_REP_DEVINFO);
    wr16(out + 12, vid);
    wr16(out + 14, pid);
    return DOCK_REPLY_LEN;
}

/* AP2 checksum (u32 BE at 4) left 0: the host only prints it. Version: 8 ASCII bytes at 8. */
size_t dockproto_deck_firmware(uint8_t out[DOCK_REPLY_LEN], const char *version)
{
    host_report(out, DOCK_REP_FIRMWARE);
    const size_t n = bounded_len(version, 8);
    if (n) memcpy(out + 8, version, n);
    return DOCK_REPLY_LEN;
}

/* Length at 3, text from 4. */
size_t dockproto_serial(uint8_t out[DOCK_REPLY_LEN], const char *serial)
{
    host_report(out, DOCK_REP_SERIAL);
    const size_t n = bounded_len(serial, DOCK_REPLY_LEN - 4);
    out[3] = (uint8_t)n;
    if (n) memcpy(out + 4, serial, n);
    return DOCK_REPLY_LEN;
}

size_t dockproto_mac(uint8_t out[DOCK_REPLY_LEN], const uint8_t mac[6])
{
    host_report(out, DOCK_REP_MAC);
    memcpy(out + 4, mac, 6);
    return DOCK_REPLY_LEN;
}

size_t dockproto_not_present(uint8_t out[DOCK_REPLY_LEN], uint8_t report_id)
{
    host_report(out, report_id);
    return DOCK_REPLY_LEN;
}

size_t dockproto_no_child(uint8_t out[DOCK_CHILD_LEN])
{
    memset(out, 0, DOCK_CHILD_LEN);
    out[0] = CHILD_PUSH0;
    out[1] = CHILD_PUSH1; /* status at 4 stays 0: no device (0x02 would be one) */
    return DOCK_CHILD_LEN;
}

size_t dockproto_keepalive(uint8_t out[DOCK_REPLY_LEN], uint8_t conn_no)
{
    memset(out, 0, DOCK_REPLY_LEN);
    out[0] = 0x01;
    out[1] = KEEPALIVE1;
    out[5] = conn_no;
    return DOCK_REPLY_LEN;
}

dock_msg_kind_t dockproto_classify(const cora_msg_t *m, uint8_t *report_id)
{
    const uint8_t *p = m->payload;
    if ((m->flags & CORA_F_ACK_NAK) && m->len >= 2 && p[0] == HOST_REPORT && p[1] == ACK1) return DOCK_MSG_ACK;
    if (m->flags & CORA_F_VERBATIM) {
        if (m->len < 1) return DOCK_MSG_OTHER;
        if (report_id) *report_id = p[0];
        switch (m->hid_op) {
        case CORA_OP_WRITE: return DOCK_MSG_CHILD_WRITE;
        case CORA_OP_SEND_REPORT: return DOCK_MSG_CHILD_SET;
        case CORA_OP_GET_REPORT: return DOCK_MSG_CHILD_GET;
        default: return DOCK_MSG_OTHER;
        }
    }
    if (m->hid_op == CORA_OP_GET_REPORT && m->len >= 2 && p[0] == HOST_REPORT) {
        if (report_id) *report_id = p[1];
        return DOCK_MSG_HOST_GET;
    }
    return DOCK_MSG_OTHER;
}

dock_deck_act_t dockproto_deck_message(const dock_deck_t *deck, const cora_msg_t *m, uint8_t out[DOCK_CHILD_LEN],
                                       size_t *out_len, uint8_t *report_id)
{
    uint8_t id = 0;
    const dock_msg_kind_t kind = dockproto_classify(m, &id);
    if (report_id) *report_id = id;
    switch (kind) {
    case DOCK_MSG_CHILD_WRITE: return DOCK_DECK_WRITE;
    case DOCK_MSG_CHILD_SET: return DOCK_DECK_SET;
    case DOCK_MSG_HOST_GET: break;
    default: return DOCK_DECK_IGNORE; /* ACKs, verbatim reads (the probes 0x08 / 0xa1), the rest */
    }
    size_t n;
    switch (id) {
    case DOCK_REP_DEVINFO: n = dockproto_devinfo(out, deck->vid, deck->pid); break;
    case DOCK_REP_FIRMWARE: n = dockproto_deck_firmware(out, deck->firmware); break;
    case DOCK_REP_SERIAL: n = dockproto_serial(out, deck->serial); break;
    case DOCK_REP_MAC: n = dockproto_mac(out, deck->mac); break;
    case DOCK_REP_ENC_AP2:
    case DOCK_REP_ENC_LD: n = dockproto_not_present(out, id); break;
    case DOCK_REP_CHILD: n = dockproto_no_child(out); break;
    default: return DOCK_DECK_READ; /* the deck's own feature report */
    }
    if (out_len) *out_len = n;
    return DOCK_DECK_ANSWER;
}

/* The longest prefix of s, at most max bytes, that doesn't end inside a UTF-8 character. */
static size_t utf8_cut(const char *s, size_t max)
{
    size_t n = bounded_len(s, max);
    if (s[n] == '\0') return n;                       /* all of it fits */
    while (n > 0 && ((uint8_t)s[n] & 0xc0) == 0x80) n--; /* s[n] continues a character: drop it whole */
    return n;
}

static const char *short_model(const char *model)
{
    static const char prefix[] = "Stream Deck ";
    const size_t p = sizeof prefix - 1;
    if (strncmp(model, prefix, p) == 0 && isalnum((unsigned char)model[p])) return model + p;
    return model;
}

void dockproto_mdns_name(char out[DOCK_MDNS_NAME_MAX + 1], const char *model, int dock, bool dup,
                         const char *box_name)
{
    char m[48], tag[24] = ""; /* " (dock " + any int + ")" */
    snprintf(m, sizeof m, "%s", model && model[0] ? short_model(model) : "Stream Deck");
    if (dup) snprintf(tag, sizeof tag, " (dock %d)", dock + 1); /* counted from 1, as the web page */
    static const char at[] = " @ ";
    const size_t fixed = strlen(tag) + sizeof at - 1;
    const char *box = box_name ? box_name : "";
    const size_t lb = utf8_cut(box, DOCK_MDNS_NAME_MAX - fixed - 1); /* leaves the model one byte */
    const size_t lm = utf8_cut(m, DOCK_MDNS_NAME_MAX - fixed - lb);  /* the model gives way first */
    snprintf(out, DOCK_MDNS_NAME_MAX + 1, "%.*s%s%s%.*s", (int)lm, m, tag, at, (int)lb, box);
}

int dockproto_pick_dock(const bool *busy, const char *const *map, int n, const char *serial, bool sticky)
{
    if (sticky && serial && serial[0])
        for (int i = 0; i < n; i++)
            if (!busy[i] && map[i] && strcmp(map[i], serial) == 0) return i; /* its dock before */
    for (int i = 0; i < n; i++)
        if (!busy[i] && (!map[i] || !map[i][0])) return i; /* nobody's */
    for (int i = 0; i < n; i++)
        if (!busy[i]) return i; /* an absent deck's, as the last resort */
    return -1;
}

void dockproto_docks_wanted(const dock_in_t *in, int n, bool mdns_on, const char *box_name, dock_want_t *out)
{
    for (int i = 0; i < n; i++) {
        dock_want_t *w = &out[i];
        memset(w, 0, sizeof *w);
        w->listen = in[i].has;
        w->announce = in[i].has && mdns_on;
    }
    /* A name two docks would share gets each one's dock number. Decided on the plain names, so a model
     * cut short to fit is caught too; a tag is never cut, so tagged names differ. each plain name built
     * once (in its dock's out[].name), compared pairwise, and built again with the tag only for a duplicate;
     * out[].sn[0] marks a duplicate until sn is filled. */
    for (int i = 0; i < n; i++)
        if (out[i].announce) dockproto_mdns_name(out[i].name, in[i].model, i, false, box_name);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n && out[i].announce; j++)
            if (out[j].announce && strcmp(out[i].name, out[j].name) == 0) out[i].sn[0] = out[j].sn[0] = 1;
    for (int i = 0; i < n; i++) {
        if (!out[i].announce) continue;
        if (out[i].sn[0]) dockproto_mdns_name(out[i].name, in[i].model, i, true, box_name);
        snprintf(out[i].sn, sizeof out[i].sn, "%s", in[i].serial ? in[i].serial : "");
    }
}

dock_act_t dockproto_dock_reconcile(const dock_want_t *want, const dock_now_t *now)
{
    dock_act_t a = {.open = want->listen && !now->listening, .close = !want->listen && now->listening};
    a.withdraw = now->announced &&
                 (!want->announce || strcmp(want->name, now->name ? now->name : "") != 0 ||
                  strcmp(want->sn, now->sn ? now->sn : "") != 0);
    a.add = want->announce && (!now->announced || a.withdraw);
    return a;
}

int dockproto_ndocks(int deck_limit, int max)
{
    const int n = deck_limit >= 1 ? deck_limit : 2;
    return n < max ? n : max;
}
