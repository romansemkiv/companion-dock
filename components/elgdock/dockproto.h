#pragma once
/* Network Dock emulation, pure C: what the box's dock ports send and a classifier for what the host
 * sends them. Layouts as @elgato-stream-deck/tcp 7.8.0 reads them (MIT;
 * packages/tcp/src/device2Info.ts, hid-device/cora.ts, connectionManager.ts, tcpWrapper.ts,
 * socketWrapper.ts, and core/src/services/properties/network-dock.ts, all-firmware.ts).
 *
 * Each dock has one TCP port, open only while a deck is assigned to it, on which the deck itself
 * is the device: its 0x80 answer carries the deck's real VID/PID, so the host builds that
 * model's driver straight away (findModelByUsb) and Companion lists one surface, the deck
 * (dockproto_deck_message). No empty Network Dock is ever presented (dockproto_dock_plan).
 * The host asks a primary GET_REPORT [0x03, id] (no VERBATIM flag) for 0x80 device info, 0x83
 * firmware, 0x84 serial, 0x85 MAC, 0x86 / 0x8a encoder firmware and 0x1c child info, and the deck's
 * own driver reads the deck's feature reports the same way. Output reports and feature reports to set
 * come verbatim (VERBATIM flag): WRITE = output report, SEND_REPORT = SET_REPORT(Feature). Input
 * reports go to the host starting with their report ID 0x01. A keepalive 01 0a ... goes out every
 * DOCK_KEEPALIVE_MS, which the host ACKs with 03 1a <connection number>; the host drops a connection
 * after 5 s without data. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cora.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DOCK_KEEPALIVE_MS 1000

/* GET_REPORT [0x03, id] of the device on a dock's port */
#define DOCK_REP_DEVINFO 0x80
#define DOCK_REP_FIRMWARE 0x83
#define DOCK_REP_SERIAL 0x84
#define DOCK_REP_MAC 0x85
#define DOCK_REP_ENC_AP2 0x86 /* encoder firmware (Studio): all-firmware.ts reads both, wants 0x18 */
#define DOCK_REP_ENC_LD 0x8a
#define DOCK_REP_CHILD 0x1c

/* The host sends both to a port it doesn't know yet, as GET_REPORT verbatim, racing 0x80: an answer
 * to either says "this is a secondary port" (hid-device/cora.ts getDeviceInfo), so a primary leaves
 * them unanswered. */
#define DOCK_PROBE_GEN2 0x08
#define DOCK_PROBE_MINI 0xa1

#define DOCK_REPLY_LEN 32  /* 0x80 / 0x83 / 0x84 / 0x85 / 0x86 / 0x8a answers, keepalive */
#define DOCK_CHILD_LEN 128 /* 0x1c answer */
#define DOCK_MDNS_NAME_MAX 63 /* an mDNS instance name (one DNS label) */

/* What a dock with a deck presents on its port: the deck. */
typedef struct {
    uint16_t vid, pid;
    const char *serial;   /* the deck's USB serial */
    const char *firmware; /* the deck's firmware as read at open ("2.02.000"), sent as it is */
    const uint8_t *mac;   /* the box's, 6 bytes */
} dock_deck_t;

/* Builders: each fills its whole buffer and returns its length. */
size_t dockproto_devinfo(uint8_t out[DOCK_REPLY_LEN], uint16_t vid, uint16_t pid);
/* 0x83: the deck's version, the first 8 characters as they are, NUL padded. Companion checks only
 * Studio and Network Dock firmware for updates, so a deck's version is never compared. */
size_t dockproto_deck_firmware(uint8_t out[DOCK_REPLY_LEN], const char *version);
size_t dockproto_serial(uint8_t out[DOCK_REPLY_LEN], const char *serial);
size_t dockproto_mac(uint8_t out[DOCK_REPLY_LEN], const uint8_t mac[6]);
/* [0x03, id, 0...]: "not present" for 0x86 / 0x8a. The host reads both at once with 0x83
 * (getAllFirmwareVersions) and keeps an encoder version only when byte 2 or 3 (0 or 1 of
 * data.slice(2)) is 0x18 (core all-firmware.ts parseAllFirmwareVersionsHelper). */
size_t dockproto_not_present(uint8_t out[DOCK_REPLY_LEN], uint8_t report_id);
/* 0x1c: nothing connected, in the 01 0b child layout (status at 4 not 0x02, so the host opens no
 * secondary connection; device2Info.ts). */
size_t dockproto_no_child(uint8_t out[DOCK_CHILD_LEN]);
size_t dockproto_keepalive(uint8_t out[DOCK_REPLY_LEN], uint8_t conn_no);

typedef enum {
    DOCK_MSG_OTHER,       /* nothing a dock acts on */
    DOCK_MSG_ACK,         /* keepalive ACK 03 1a <connection number> */
    DOCK_MSG_HOST_GET,    /* GET_REPORT [0x03, id] */
    DOCK_MSG_CHILD_WRITE, /* verbatim output report */
    DOCK_MSG_CHILD_SET,   /* verbatim SET_REPORT(Feature), payload[0] = report ID */
    DOCK_MSG_CHILD_GET,   /* verbatim GET_REPORT(Feature) of report ID payload[0] */
} dock_msg_kind_t;

/* *report_id (may be NULL) is set for HOST_GET and CHILD_GET / CHILD_SET. */
dock_msg_kind_t dockproto_classify(const cora_msg_t *m, uint8_t *report_id);

/* What a dock's port does with a host message while it presents a deck. */
typedef enum {
    DOCK_DECK_IGNORE, /* left unanswered: ACKs (the data shows the host is there), the probes 0x08 /
                       * 0xa1 (an answer would make the port a secondary) and every other verbatim read
                       * (the host never sends one to a primary), anything else */
    DOCK_DECK_ANSWER, /* out holds the port's own answer, to send flagged RESULT: 0x80 with the deck's
                       * VID/PID, 0x83 its firmware, 0x84 its serial, 0x85 the box's MAC, 0x86 / 0x8a
                       * not present, 0x1c nothing connected */
    DOCK_DECK_READ,   /* any other GET_REPORT [0x03, id]: the deck's driver reading feature report id
                       * (gen2 0x05 / 0x06, gen1 0x03 / 0x04): answer the deck's report itself, flagged
                       * VERBATIM | RESULT, as the host keys a verbatim answer by its first byte, the
                       * report ID (hid-device/cora.ts:56-58) */
    DOCK_DECK_WRITE,  /* verbatim WRITE: an output report to pass on */
    DOCK_DECK_SET,    /* verbatim SEND_REPORT: a feature report to set (brightness 03 08 <pct>, ...) */
} dock_deck_act_t;

/* *out_len (ANSWER) and *report_id (READ, SET) are set as the action needs; out holds at least
 * DOCK_CHILD_LEN bytes. */
dock_deck_act_t dockproto_deck_message(const dock_deck_t *deck, const cora_msg_t *m, uint8_t out[DOCK_CHILD_LEN],
                                       size_t *out_len, uint8_t *report_id);

/* ---- A dock listens and is announced only while a deck is assigned to it (dock 0 too) ---- */

/* One dock as the assignments have it. */
typedef struct {
    bool has;           /* a deck is assigned */
    const char *model;  /* its model name (deckmodel), when has */
    const char *serial; /* its USB serial, when has */
} dock_in_t;

/* What a dock should be doing. */
typedef struct {
    bool listen;                       /* its port open */
    bool announce;                     /* announced as _elg._tcp */
    char name[DOCK_MDNS_NAME_MAX + 1]; /* the instance name, when announce ("" otherwise) */
    char sn[32];                       /* TXT sn: the deck's serial, when announce */
} dock_want_t;

/* The wanted state of n docks: a dock listens while it has a deck, and is announced while it has one
 * and mdns_on. Names come from dockproto_mdns_name and never repeat among the docks: a name that two
 * docks would share gets each one's dock number (" (dock <k+1>)"). */
void dockproto_docks_wanted(const dock_in_t *in, int n, bool mdns_on, const char *box_name, dock_want_t *out);

/* A dock as it is now. */
typedef struct {
    bool listening; /* its port is open */
    bool announced; /* name / sn are announced */
    const char *name, *sn;
} dock_now_t;

/* The steps from now to want. The server task runs every dock's open / close, then every withdraw,
 * then every add (an add only once the port is open), so two docks never hold one name, even for a
 * moment: wanted names are unique, and after the withdraws every name still announced is its dock's
 * wanted one. */
typedef struct {
    bool open, close; /* the port */
    bool withdraw;    /* the announcement: no longer wanted, or another name or sn */
    bool add;         /* announce want->name / want->sn */
} dock_act_t;

dock_act_t dockproto_dock_reconcile(const dock_want_t *want, const dock_now_t *now);

/* The mDNS instance name of a dock: "<short model> @ <box name>", e.g. "Module 32 @
 * CompanionBox-8D1C" ("Stream Deck" when model is NULL or ""). The short model is the name without
 * "Stream Deck " when what remains starts with a letter or digit ("Stream Deck +" stays). dup: another
 * dock of the box would have the same name; " (dock <dock + 1>)" follows the model, counting docks from
 * 1 as the web page does, as instance names must differ. At most DOCK_MDNS_NAME_MAX bytes: the model part is cut first, the box
 * name only when it leaves no room for one character of it (always at a UTF-8 character boundary). */
void dockproto_mdns_name(char out[DOCK_MDNS_NAME_MAX + 1], const char *model, int dock, bool dup,
                         const char *box_name);

/* Which of n docks a deck that just attached takes: the dock it reserved before, if free; else
 * the lowest free dock nobody reserved; else (every unreserved dock is taken) the lowest free dock a
 * deck that isn't here reserved. -1 when every dock is busy. busy[i]: dock i has a deck; map[i]: the
 * deck serial that reserved dock i (the NVS dock map; "" or NULL for none); sticky: serial is a real
 * serial, which may own a reservation (usbdeck's "unknown-<addr>" stand-in doesn't). */
int dockproto_pick_dock(const bool *busy, const char *const *map, int n, const char *serial, bool sticky);

/* the docks the box runs for its deck limit (deckLimit, 2 or 4): one per deck, at most max (ELGDOCK_MAX);
 * anything below 1 is the default, 2. Docks 0 .. n - 1, ports 5343 + 2k, are the only ones ever opened or announced. */
int dockproto_ndocks(int deck_limit, int max);

#ifdef __cplusplus
}
#endif
