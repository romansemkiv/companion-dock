#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "dockproto.h"

void setUp(void) {}
void tearDown(void) {}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static const uint8_t MAC[6] = {0x30, 0xed, 0xa0, 0x1a, 0x2b, 0x3c};

static void test_devinfo_vid_pid(void)
{
    uint8_t out[DOCK_REPLY_LEN];
    TEST_ASSERT_EQUAL_size_t(DOCK_REPLY_LEN, dockproto_devinfo(out, 0x0fd9, 0x008f));
    TEST_ASSERT_EQUAL_HEX8(0x03, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, out[1]);
    TEST_ASSERT_EQUAL_HEX16(0x0fd9, rd16(out + 12));
    TEST_ASSERT_EQUAL_HEX16(0x008f, rd16(out + 14));
}

static void test_no_child(void)
{
    uint8_t out[DOCK_CHILD_LEN];
    memset(out, 0xee, sizeof out);
    TEST_ASSERT_EQUAL_size_t(DOCK_CHILD_LEN, dockproto_no_child(out));
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]); /* the 01 0b layout answers 0x1c */
    TEST_ASSERT_EQUAL_HEX8(0x0b, out[1]);
    for (int i = 2; i < DOCK_CHILD_LEN; i++) TEST_ASSERT_EQUAL_HEX8(0, out[i]); /* status 0: nothing */
}

static void test_serial_layout(void)
{
    uint8_t out[DOCK_REPLY_LEN];
    TEST_ASSERT_EQUAL_size_t(DOCK_REPLY_LEN, dockproto_serial(out, "AB1CD2345678EF"));
    TEST_ASSERT_EQUAL_HEX8(0x03, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x84, out[1]);
    TEST_ASSERT_EQUAL_UINT8(14, out[3]); /* length at 3, text from 4 */
    TEST_ASSERT_EQUAL_STRING_LEN("AB1CD2345678EF", out + 4, 14);
}

static void test_mac_layout(void)
{
    uint8_t out[DOCK_REPLY_LEN];
    TEST_ASSERT_EQUAL_size_t(DOCK_REPLY_LEN, dockproto_mac(out, MAC));
    TEST_ASSERT_EQUAL_HEX8(0x03, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x85, out[1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(MAC, out + 4, 6);
}

static void test_keepalive_payload(void)
{
    uint8_t out[DOCK_REPLY_LEN];
    const size_t n = dockproto_keepalive(out, 7);
    TEST_ASSERT_TRUE(n > 4); /* the host only takes a payload longer than 4 bytes as a keepalive */
    TEST_ASSERT_EQUAL_size_t(DOCK_REPLY_LEN, n);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0a, out[1]);
    TEST_ASSERT_EQUAL_UINT8(7, out[5]); /* connection number, echoed in the ACK */
}

static cora_msg_t msg(uint16_t flags, uint8_t op, const uint8_t *pl, uint32_t len)
{
    const cora_msg_t m = {.flags = flags, .hid_op = op, .msg_id = 42, .payload = pl, .len = len};
    return m;
}

/* A new connection to a dock's port, after the box's keepalive: the host answers in Cora framing (its first 4 bytes
 * the magic) and takes the deck; a close, other bytes (a port scan's probe, HTTP, the legacy framing the box never
 * speaks), or silence for DOCK_VET_MS are turned away, the connection being served untouched */
static void test_vet(void)
{
    const uint8_t cora[] = {0x43, 0x93, 0x8a, 0x41, 0x02, 0x00};
    TEST_ASSERT_EQUAL(DOCK_VET_ACCEPT, dockproto_vet(cora, 4, false, 10));
    TEST_ASSERT_EQUAL(DOCK_VET_ACCEPT, dockproto_vet(cora, 6, false, 2999));
    TEST_ASSERT_EQUAL(DOCK_VET_ACCEPT, dockproto_vet(cora, 4, true, 10)); /* said it, then closed: the session finds out */
    TEST_ASSERT_EQUAL(DOCK_VET_WAIT, dockproto_vet(cora, 0, false, 0));
    TEST_ASSERT_EQUAL(DOCK_VET_WAIT, dockproto_vet(cora, 2, false, 100)); /* a partial magic: more to come */
    TEST_ASSERT_EQUAL(DOCK_VET_WAIT, dockproto_vet(cora, 3, false, DOCK_VET_MS - 1));
    TEST_ASSERT_EQUAL(DOCK_VET_REJECT, dockproto_vet(cora, 3, false, DOCK_VET_MS));
    TEST_ASSERT_EQUAL(DOCK_VET_REJECT, dockproto_vet(cora, 0, false, DOCK_VET_MS));
    TEST_ASSERT_EQUAL(DOCK_VET_REJECT, dockproto_vet(cora, 0, true, 5)); /* nc -z: connect, close */
    TEST_ASSERT_EQUAL(DOCK_VET_REJECT, dockproto_vet(cora, 2, true, 5));
    TEST_ASSERT_EQUAL(DOCK_VET_REJECT, dockproto_vet((const uint8_t *)"GET / HTTP/1.1", 14, false, 5));
    TEST_ASSERT_EQUAL(DOCK_VET_REJECT, dockproto_vet((const uint8_t *)"\x43\x93\x00", 3, false, 5));
    TEST_ASSERT_EQUAL(DOCK_VET_REJECT, dockproto_vet((const uint8_t *)"\x01\x0a\x00\x00", 4, false, 5));
    TEST_ASSERT_EQUAL(DOCK_VET_REJECT, dockproto_vet((const uint8_t *)"\r\n", 2, false, 5));
    TEST_ASSERT_EQUAL(DOCK_VET_REJECT, dockproto_vet(NULL, 4, false, 5));
}

static void test_classify_ack(void)
{
    uint8_t ack[32] = {0x03, 0x1a, 0x07};
    const cora_msg_t m = msg(CORA_F_ACK_NAK, CORA_OP_WRITE, ack, sizeof ack);
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_ACK, dockproto_classify(&m, NULL));
}

static void test_classify_host_get(void)
{
    const uint8_t pl[2] = {0x03, 0x84};
    const cora_msg_t m = msg(CORA_F_NONE, CORA_OP_GET_REPORT, pl, 2);
    uint8_t id = 0;
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_HOST_GET, dockproto_classify(&m, &id));
    TEST_ASSERT_EQUAL_HEX8(0x84, id);
}

static void test_classify_child_traffic(void)
{
    uint8_t rep[1024] = {0x02, 0x07, 0x05, 0x01};
    uint8_t id = 0;
    cora_msg_t m = msg(CORA_F_VERBATIM, CORA_OP_WRITE, rep, sizeof rep);
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_CHILD_WRITE, dockproto_classify(&m, &id));

    const uint8_t bright[32] = {0x03, 0x08, 60};
    m = msg(CORA_F_VERBATIM, CORA_OP_SEND_REPORT, bright, sizeof bright);
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_CHILD_SET, dockproto_classify(&m, &id));
    TEST_ASSERT_EQUAL_HEX8(0x03, id);

    const uint8_t get[1] = {0x06};
    m = msg(CORA_F_VERBATIM, CORA_OP_GET_REPORT, get, 1);
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_CHILD_GET, dockproto_classify(&m, &id));
    TEST_ASSERT_EQUAL_HEX8(0x06, id);
}

static void test_classify_other(void)
{
    const uint8_t one[1] = {0x80}, dev[2] = {0x03, 0x80};
    cora_msg_t m = msg(CORA_F_VERBATIM, CORA_OP_GET_REPORT, one, 0); /* no report ID */
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_OTHER, dockproto_classify(&m, NULL));
    m = msg(CORA_F_NONE, CORA_OP_GET_REPORT, one, 1); /* host GET without the 0x03 prefix */
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_OTHER, dockproto_classify(&m, NULL));
    m = msg(CORA_F_NONE, CORA_OP_WRITE, dev, 2); /* not a GET, not verbatim */
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_OTHER, dockproto_classify(&m, NULL));
    m = msg(CORA_F_VERBATIM, CORA_OP_WRITE, dev, 0); /* empty output report */
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_OTHER, dockproto_classify(&m, NULL));
    m = msg(CORA_F_VERBATIM, 0x07, dev, 2); /* unknown hidOp */
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_OTHER, dockproto_classify(&m, NULL));
}

static void test_classify_ack_needs_flag(void)
{
    uint8_t ack[32] = {0x03, 0x1a, 0x07};
    cora_msg_t m = msg(CORA_F_NONE, CORA_OP_WRITE, ack, sizeof ack);
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_OTHER, dockproto_classify(&m, NULL));
    m = msg(CORA_F_VERBATIM, CORA_OP_WRITE, ack, sizeof ack); /* an output report starting 03 1a */
    TEST_ASSERT_EQUAL_INT(DOCK_MSG_CHILD_WRITE, dockproto_classify(&m, NULL));
}

/* ---- a dock with a deck presents the deck itself ---- */

static const dock_deck_t DECK = {
    .vid = 0x0fd9, .pid = 0x00ba, .serial = "AB1CD2345678EF", .firmware = "2.02.000", .mac = MAC};

static dock_deck_act_t deck_get(uint8_t id, uint8_t *out, size_t *n)
{
    const uint8_t pl[2] = {0x03, id};
    const cora_msg_t m = {.flags = CORA_F_NONE, .hid_op = CORA_OP_GET_REPORT, .msg_id = 7, .payload = pl, .len = 2};
    uint8_t rid = 0;
    *n = 0;
    memset(out, 0xee, DOCK_CHILD_LEN);
    return dockproto_deck_message(&DECK, &m, out, n, &rid);
}

/* 0x80 in a Network Dock's layout with the deck's real IDs: the host builds the deck's own driver
 * (findModelByUsb) and Companion lists the deck alone. */
static void test_deck_devinfo(void)
{
    uint8_t out[DOCK_CHILD_LEN];
    size_t n;
    TEST_ASSERT_EQUAL_INT(DOCK_DECK_ANSWER, deck_get(DOCK_REP_DEVINFO, out, &n));
    TEST_ASSERT_EQUAL_size_t(DOCK_REPLY_LEN, n);
    TEST_ASSERT_EQUAL_HEX8(0x03, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, out[1]); /* the host keys a non-verbatim answer by byte 1 */
    TEST_ASSERT_EQUAL_HEX16(0x0fd9, rd16(out + 12));
    TEST_ASSERT_EQUAL_HEX16(0x00ba, rd16(out + 14));
    TEST_ASSERT_EQUAL_HEX8(0x00, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[31]);
}

static void test_deck_firmware_serial_mac(void)
{
    uint8_t out[DOCK_CHILD_LEN];
    size_t n;
    TEST_ASSERT_EQUAL_INT(DOCK_DECK_ANSWER, deck_get(DOCK_REP_FIRMWARE, out, &n));
    TEST_ASSERT_EQUAL_size_t(DOCK_REPLY_LEN, n);
    TEST_ASSERT_EQUAL_HEX8(0x83, out[1]);
    TEST_ASSERT_EQUAL_STRING_LEN("2.02.000", out + 8, 8); /* the deck's own, dots and all */
    TEST_ASSERT_EQUAL_INT(DOCK_DECK_ANSWER, deck_get(DOCK_REP_SERIAL, out, &n));
    TEST_ASSERT_EQUAL_HEX8(0x84, out[1]);
    TEST_ASSERT_EQUAL_UINT8(14, out[3]);
    TEST_ASSERT_EQUAL_STRING_LEN("AB1CD2345678EF", out + 4, 14);
    TEST_ASSERT_EQUAL_INT(DOCK_DECK_ANSWER, deck_get(DOCK_REP_MAC, out, &n));
    TEST_ASSERT_EQUAL_HEX8(0x85, out[1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(MAC, out + 4, 6);
}

/* getAllFirmwareVersions reads 0x83, 0x86 and 0x8a at once: all three must answer, and the encoder
 * ones say "none" (no 0x18 at 0 or 1 of data.slice(2)). */
static void test_deck_encoder_firmware_absent(void)
{
    static const uint8_t ids[2] = {DOCK_REP_ENC_AP2, DOCK_REP_ENC_LD};
    for (int i = 0; i < 2; i++) {
        uint8_t out[DOCK_CHILD_LEN];
        size_t n;
        TEST_ASSERT_EQUAL_INT(DOCK_DECK_ANSWER, deck_get(ids[i], out, &n));
        TEST_ASSERT_EQUAL_size_t(DOCK_REPLY_LEN, n);
        TEST_ASSERT_EQUAL_HEX8(0x03, out[0]);
        TEST_ASSERT_EQUAL_HEX8(ids[i], out[1]);
        for (int b = 2; b < DOCK_REPLY_LEN; b++) TEST_ASSERT_EQUAL_HEX8(0, out[b]);
    }
}

/* The library asks a primary for its child right after it connects: nothing, so no secondary. */
static void test_deck_no_child(void)
{
    uint8_t out[DOCK_CHILD_LEN];
    size_t n;
    TEST_ASSERT_EQUAL_INT(DOCK_DECK_ANSWER, deck_get(DOCK_REP_CHILD, out, &n));
    TEST_ASSERT_EQUAL_size_t(DOCK_CHILD_LEN, n);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]); /* the host routes 01 0b to its pending 0x1c read */
    TEST_ASSERT_EQUAL_HEX8(0x0b, out[1]);
    TEST_ASSERT_NOT_EQUAL(0x02, out[4]);
    TEST_ASSERT_EQUAL_UINT16(0, rd16(out + 126));
}

/* Which requests are answered on a primary with a deck, and how the rest goes. */
static void test_deck_message_routes(void)
{
    uint8_t out[DOCK_CHILD_LEN];
    size_t n = 0;
    uint8_t rid = 0;

    /* the deck driver's own feature reads come as GET_REPORT [0x03, id]: the deck's report */
    static const uint8_t reads[4] = {0x05, 0x06, 0x03, 0x04};
    for (int i = 0; i < 4; i++) {
        const uint8_t pl[2] = {0x03, reads[i]};
        const cora_msg_t m = msg(CORA_F_NONE, CORA_OP_GET_REPORT, pl, 2);
        rid = 0;
        TEST_ASSERT_EQUAL_INT(DOCK_DECK_READ, dockproto_deck_message(&DECK, &m, out, &n, &rid));
        TEST_ASSERT_EQUAL_HEX8(reads[i], rid);
    }

    /* the identification race's verbatim probes, and any verbatim read: never answered here */
    static const uint8_t probes[3] = {DOCK_PROBE_GEN2, DOCK_PROBE_MINI, 0x06};
    for (int i = 0; i < 3; i++) {
        const cora_msg_t m = msg(CORA_F_VERBATIM, CORA_OP_GET_REPORT, &probes[i], 1);
        TEST_ASSERT_EQUAL_INT(DOCK_DECK_IGNORE, dockproto_deck_message(&DECK, &m, out, &n, &rid));
    }

    uint8_t rep[1024] = {0x02, 0x07, 0x05, 0x01};
    cora_msg_t m = msg(CORA_F_VERBATIM, CORA_OP_WRITE, rep, sizeof rep);
    TEST_ASSERT_EQUAL_INT(DOCK_DECK_WRITE, dockproto_deck_message(&DECK, &m, out, &n, &rid));

    const uint8_t bright[32] = {0x03, 0x08, 60};
    m = msg(CORA_F_VERBATIM, CORA_OP_SEND_REPORT, bright, sizeof bright);
    rid = 0;
    TEST_ASSERT_EQUAL_INT(DOCK_DECK_SET, dockproto_deck_message(&DECK, &m, out, &n, &rid));
    TEST_ASSERT_EQUAL_HEX8(0x03, rid);

    uint8_t ack[32] = {0x03, 0x1a, 0x07};
    m = msg(CORA_F_ACK_NAK, CORA_OP_WRITE, ack, sizeof ack);
    TEST_ASSERT_EQUAL_INT(DOCK_DECK_IGNORE, dockproto_deck_message(&DECK, &m, out, &n, &rid));

    const uint8_t dev[2] = {0x03, 0x80};
    m = msg(CORA_F_NONE, CORA_OP_WRITE, dev, 2); /* not a GET, not verbatim */
    TEST_ASSERT_EQUAL_INT(DOCK_DECK_IGNORE, dockproto_deck_message(&DECK, &m, out, &n, &rid));
}

static void test_deck_firmware_builder(void)
{
    uint8_t out[DOCK_REPLY_LEN];
    TEST_ASSERT_EQUAL_size_t(DOCK_REPLY_LEN, dockproto_deck_firmware(out, "1.01.017-long"));
    TEST_ASSERT_EQUAL_HEX8(0x03, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x83, out[1]);
    TEST_ASSERT_EQUAL_STRING_LEN("1.01.017", out + 8, 8);
    TEST_ASSERT_EQUAL_HEX8(0, out[16]);
    dockproto_deck_firmware(out, "1.2");
    TEST_ASSERT_EQUAL_STRING_LEN("1.2", out + 8, 3);
    TEST_ASSERT_EQUAL_HEX8(0, out[11]);
    TEST_ASSERT_EQUAL_size_t(DOCK_REPLY_LEN, dockproto_not_present(out, 0x86));
    TEST_ASSERT_EQUAL_HEX8(0x03, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x86, out[1]);
}

static void test_mdns_name(void)
{
    char out[DOCK_MDNS_NAME_MAX + 1];
    dockproto_mdns_name(out, "Stream Deck Module 32", 0, false, "CompanionBox-8D1C");
    TEST_ASSERT_EQUAL_STRING("Module 32 @ CompanionBox-8D1C", out);
    dockproto_mdns_name(out, NULL, 0, false, "CompanionBox-8D1C"); /* never announced without a deck */
    TEST_ASSERT_EQUAL_STRING("Stream Deck @ CompanionBox-8D1C", out);
    dockproto_mdns_name(out, "", 3, false, "Box");
    TEST_ASSERT_EQUAL_STRING("Stream Deck @ Box", out);
    dockproto_mdns_name(out, "Stream Deck MK.2", 1, false, "Box");
    TEST_ASSERT_EQUAL_STRING("MK.2 @ Box", out);
    dockproto_mdns_name(out, "Stream Deck +", 1, false, "Box"); /* "+" alone says nothing */
    TEST_ASSERT_EQUAL_STRING("Stream Deck + @ Box", out);
    dockproto_mdns_name(out, "Stream Deck + XL", 1, false, "Box");
    TEST_ASSERT_EQUAL_STRING("Stream Deck + XL @ Box", out);
    dockproto_mdns_name(out, "Stream Deck", 1, false, "Box"); /* the original: nothing to cut */
    TEST_ASSERT_EQUAL_STRING("Stream Deck @ Box", out);
    dockproto_mdns_name(out, "Stream Deck XL", 2, true, "Box"); /* a second XL on the box */
    TEST_ASSERT_EQUAL_STRING("XL (dock 3) @ Box", out);    /* docks counted from 1, as the web page */
}

static void test_mdns_name_limit(void)
{
    char out[DOCK_MDNS_NAME_MAX + 1];
    /* the longest model, a tag and the longest box name (32 bytes): 64 bytes; the model gives one */
    const char *box = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    dockproto_mdns_name(out, "Stream Deck Mini Discord Edition", 5, true, box);
    TEST_ASSERT_EQUAL_size_t(DOCK_MDNS_NAME_MAX, strlen(out));
    TEST_ASSERT_EQUAL_STRING("Mini Discord Editio (dock 6) @ ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", out);

    /* a box name too long for any model: the model keeps one character, the box name is cut */
    char longbox[80];
    memset(longbox, 'b', sizeof longbox - 1);
    longbox[sizeof longbox - 1] = '\0';
    dockproto_mdns_name(out, "Stream Deck Module 32", 0, false, longbox);
    TEST_ASSERT_EQUAL_size_t(DOCK_MDNS_NAME_MAX, strlen(out));
    TEST_ASSERT_EQUAL_STRING_LEN("M @ bbb", out, 7);

    /* never half a UTF-8 character: a box name of 30 two-byte letters (60 bytes) is cut to 29 of them
     * (58 bytes, not 59), and the model takes the 2 bytes left */
    char utf[61] = {0};
    for (int i = 0; i < 30; i++) {
        utf[2 * i] = (char)0xc3;
        utf[2 * i + 1] = (char)0xa9; /* e acute */
    }
    dockproto_mdns_name(out, "Stream Deck Neo", 0, false, utf);
    TEST_ASSERT_EQUAL_size_t(DOCK_MDNS_NAME_MAX, strlen(out));
    TEST_ASSERT_EQUAL_HEX8(0xa9, (uint8_t)out[DOCK_MDNS_NAME_MAX - 1]); /* ends on a whole character */
    TEST_ASSERT_EQUAL_STRING_LEN("Ne @ ", out, 5);
}

/* ---- A dock listens and is announced only while a deck is assigned ---- */

static void test_docks_wanted(void)
{
    const dock_in_t in[4] = {{true, "Stream Deck Module 32", "AB1CD2345678EF"}, {false, "", ""},
                             {true, "Stream Deck MK.2", "MK2"}, {false, NULL, NULL}};
    dock_want_t w[4];
    dockproto_docks_wanted(in, 4, true, "CompanionBox-8D1C", w);
    TEST_ASSERT_TRUE(w[0].listen);
    TEST_ASSERT_TRUE(w[0].announce);
    TEST_ASSERT_EQUAL_STRING("Module 32 @ CompanionBox-8D1C", w[0].name);
    TEST_ASSERT_EQUAL_STRING("AB1CD2345678EF", w[0].sn);
    TEST_ASSERT_EQUAL_STRING("MK.2 @ CompanionBox-8D1C", w[2].name);
    for (int i = 1; i < 4; i += 2) { /* no deck, dock 0 included elsewhere: closed and unannounced */
        TEST_ASSERT_FALSE(w[i].listen);
        TEST_ASSERT_FALSE(w[i].announce);
        TEST_ASSERT_EQUAL_STRING("", w[i].name);
    }
    const dock_in_t none[1] = {{false, NULL, NULL}}; /* dock 0 without a deck */
    dockproto_docks_wanted(none, 1, true, "Box", w);
    TEST_ASSERT_FALSE(w[0].listen);
    TEST_ASSERT_FALSE(w[0].announce);
    dockproto_docks_wanted(in, 4, false, "Box", w); /* mDNS off: ports open, nothing announced */
    TEST_ASSERT_TRUE(w[0].listen);
    TEST_ASSERT_FALSE(w[0].announce);
    TEST_ASSERT_EQUAL_STRING("", w[0].name);
}

/* Two decks of a model: both say their dock (from 1), whichever came first, so the names never
 * depend on the order docks are filled and never repeat. */
static void test_docks_wanted_names_unique(void)
{
    const dock_in_t in[6] = {{true, "Stream Deck XL", "X1"}, {true, "Stream Deck Module 32", "M1"},
                             {true, "Stream Deck XL", "X2"}, {false, NULL, NULL},
                             {true, "Stream Deck XL", "X3"}, {true, "Stream Deck MK.2", "K1"}};
    dock_want_t w[6];
    dockproto_docks_wanted(in, 6, true, "Box", w);
    TEST_ASSERT_EQUAL_STRING("XL (dock 1) @ Box", w[0].name);
    TEST_ASSERT_EQUAL_STRING("Module 32 @ Box", w[1].name);
    TEST_ASSERT_EQUAL_STRING("XL (dock 3) @ Box", w[2].name);
    TEST_ASSERT_EQUAL_STRING("XL (dock 5) @ Box", w[4].name);
    TEST_ASSERT_EQUAL_STRING("MK.2 @ Box", w[5].name);
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if (w[i].announce && w[j].announce) TEST_ASSERT_NOT_EQUAL(0, strcmp(w[i].name, w[j].name));
}

/* ---- The old dockproto_docks_wanted, kept here as the reference ---- */

static void ref_docks_wanted(const dock_in_t *in, int n, bool mdns_on, const char *box_name, dock_want_t *out)
{
    for (int i = 0; i < n; i++) {
        dock_want_t *w = &out[i];
        memset(w, 0, sizeof *w);
        w->listen = in[i].has;
        w->announce = in[i].has && mdns_on;
    }
    for (int i = 0; i < n; i++) {
        if (!out[i].announce) continue;
        char mine[DOCK_MDNS_NAME_MAX + 1], other[DOCK_MDNS_NAME_MAX + 1];
        dockproto_mdns_name(mine, in[i].model, i, false, box_name);
        bool dup = false;
        for (int j = 0; j < n && !dup; j++) {
            if (j == i || !out[j].announce) continue;
            dockproto_mdns_name(other, in[j].model, j, false, box_name);
            dup = strcmp(mine, other) == 0;
        }
        dockproto_mdns_name(out[i].name, in[i].model, i, dup, box_name);
        snprintf(out[i].sn, sizeof out[i].sn, "%s", in[i].serial ? in[i].serial : "");
    }
}

static uint32_t rng = 0x1B873593u;
static uint32_t rnd(void) /* xorshift32: the same cases every run */
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* Each plain name built once: the same wanted state as the old way for 20k random sets of 1..8 docks, with
 * models that repeat, models that only match once cut short for a long box name, no model, mDNS on and off. */
static void test_docks_wanted_matches_old(void)
{
    static const char *const MODEL[] = {"Stream Deck XL", "Stream Deck Module 32", "Stream Deck MK.2", "",
                                        NULL, "Stream Deck Plus XL Long Model Name A", "Stream Deck Plus XL Long Model Name B",
                                        "Stream Deck Neo"};
    static const char *const SERIAL[] = {"X1", "", NULL, "AB1CD2345678EF"};
    static const char *const BOX[] = {"Box", "CompanionBox-8D1C", "", NULL,
                                      "A-very-long-box-name-that-leaves-the-model-only-a-few-bytes-x"};
    dock_in_t in[8];
    dock_want_t a[8], b[8];
    int dups = 0;
    for (int it = 0; it < 20000; it++) {
        const int n = 1 + (int)(rnd() % 8);
        for (int i = 0; i < n; i++)
            in[i] = (dock_in_t){rnd() % 4 != 0, MODEL[rnd() % 8], SERIAL[rnd() % 4]};
        const bool on = rnd() % 5 != 0;
        const char *box = BOX[rnd() % 5];
        dockproto_docks_wanted(in, n, on, box, a);
        ref_docks_wanted(in, n, on, box, b);
        for (int i = 0; i < n; i++) {
            TEST_ASSERT_EQUAL(b[i].listen, a[i].listen);
            TEST_ASSERT_EQUAL(b[i].announce, a[i].announce);
            TEST_ASSERT_EQUAL_STRING(b[i].name, a[i].name);
            TEST_ASSERT_EQUAL_STRING(b[i].sn, a[i].sn);
            dups += strstr(a[i].name, " (dock ") != NULL;
        }
    }
    TEST_ASSERT_TRUE(dups > 10000); /* tagged names well covered */
}

static void test_dock_reconcile(void)
{
    dock_want_t w = {.listen = true, .announce = true, .name = "Module 32 @ Box", .sn = "S1"};
    dock_now_t n = {.listening = false, .announced = false, .name = "", .sn = ""};
    dock_act_t a = dockproto_dock_reconcile(&w, &n); /* a deck came: open and announce */
    TEST_ASSERT_TRUE(a.open);
    TEST_ASSERT_FALSE(a.close);
    TEST_ASSERT_FALSE(a.withdraw);
    TEST_ASSERT_TRUE(a.add);

    n = (dock_now_t){.listening = true, .announced = true, .name = "Module 32 @ Box", .sn = "S1"};
    a = dockproto_dock_reconcile(&w, &n); /* in line: nothing to do */
    TEST_ASSERT_FALSE(a.open || a.close || a.withdraw || a.add);

    n.sn = "S2"; /* another deck of the model on the dock: announced again with its serial */
    a = dockproto_dock_reconcile(&w, &n);
    TEST_ASSERT_TRUE(a.withdraw && a.add);
    TEST_ASSERT_FALSE(a.open || a.close);
    n = (dock_now_t){.listening = true, .announced = true, .name = "XL @ Box", .sn = "S1"}; /* renamed */
    a = dockproto_dock_reconcile(&w, &n);
    TEST_ASSERT_TRUE(a.withdraw && a.add);

    const dock_want_t gone = {0}; /* the deck went: close and withdraw */
    n = (dock_now_t){.listening = true, .announced = true, .name = "Module 32 @ Box", .sn = "S1"};
    a = dockproto_dock_reconcile(&gone, &n);
    TEST_ASSERT_TRUE(a.close && a.withdraw);
    TEST_ASSERT_FALSE(a.open || a.add);
    n = (dock_now_t){.listening = false, .announced = false, .name = "", .sn = ""};
    a = dockproto_dock_reconcile(&gone, &n); /* nothing there: nothing to do */
    TEST_ASSERT_FALSE(a.open || a.close || a.withdraw || a.add);

    const dock_want_t quiet = {.listen = true}; /* mDNS switched off: the port stays, the name goes */
    n = (dock_now_t){.listening = true, .announced = true, .name = "Module 32 @ Box", .sn = "S1"};
    a = dockproto_dock_reconcile(&quiet, &n);
    TEST_ASSERT_TRUE(a.withdraw);
    TEST_ASSERT_FALSE(a.open || a.close || a.add);
}

/* The server task's order (every withdraw, then every add) never has two docks holding one name,
 * across the changes that rename docks: a second XL coming to a lower dock, the first one going, a
 * deck swapped for another model, and two decks trading docks. */
#define SIM_N 4
typedef struct {
    bool announced;
    char name[DOCK_MDNS_NAME_MAX + 1], sn[32];
} sim_dock_t;

static void sim_apply(sim_dock_t *d, const dock_in_t *in)
{
    dock_want_t w[SIM_N];
    dock_act_t a[SIM_N];
    dockproto_docks_wanted(in, SIM_N, true, "Box", w);
    for (int i = 0; i < SIM_N; i++) {
        const dock_now_t n = {.listening = true, .announced = d[i].announced, .name = d[i].name, .sn = d[i].sn};
        a[i] = dockproto_dock_reconcile(&w[i], &n);
    }
    for (int i = 0; i < SIM_N; i++)
        if (a[i].withdraw) d[i].announced = false;
    for (int i = 0; i < SIM_N; i++) {
        if (!a[i].add) continue;
        for (int j = 0; j < SIM_N; j++) /* what mdns would refuse */
            if (j != i && d[j].announced) TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(d[j].name, w[i].name), w[i].name);
        d[i].announced = true;
        strcpy(d[i].name, w[i].name);
        strcpy(d[i].sn, w[i].sn);
    }
    for (int i = 0; i < SIM_N; i++) { /* in line afterwards */
        TEST_ASSERT_EQUAL(w[i].announce, d[i].announced);
        if (w[i].announce) TEST_ASSERT_EQUAL_STRING(w[i].name, d[i].name);
    }
}

static void test_reconcile_never_clashes(void)
{
    sim_dock_t d[SIM_N] = {0};
    const dock_in_t s1[SIM_N] = {{false, 0, 0}, {false, 0, 0}, {true, "Stream Deck XL", "X1"}, {false, 0, 0}};
    sim_apply(d, s1);
    TEST_ASSERT_EQUAL_STRING("XL @ Box", d[2].name);
    const dock_in_t s2[SIM_N] = {{true, "Stream Deck XL", "X2"}, {false, 0, 0}, {true, "Stream Deck XL", "X1"}, {false, 0, 0}};
    sim_apply(d, s2); /* a second XL on dock 0 */
    TEST_ASSERT_EQUAL_STRING("XL (dock 1) @ Box", d[0].name);
    TEST_ASSERT_EQUAL_STRING("XL (dock 3) @ Box", d[2].name);
    const dock_in_t s3[SIM_N] = {{true, "Stream Deck XL", "X2"}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}};
    sim_apply(d, s3); /* dock 2's XL unplugged: dock 0's XL alone again */
    TEST_ASSERT_EQUAL_STRING("XL @ Box", d[0].name);
    TEST_ASSERT_FALSE(d[2].announced);
    const dock_in_t s4[SIM_N] = {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {true, "Stream Deck XL", "X3"}};
    sim_apply(d, s4); /* one XL out, another in on dock 3 within one pass: the name moves */
    TEST_ASSERT_EQUAL_STRING("XL @ Box", d[3].name);
    TEST_ASSERT_FALSE(d[0].announced);
    const dock_in_t s5[SIM_N] = {{true, "Stream Deck XL", "X3"}, {false, 0, 0}, {false, 0, 0}, {true, "Stream Deck Neo", "N1"}};
    sim_apply(d, s5); /* a swap: the XL on dock 0, a Neo where it was */
    TEST_ASSERT_EQUAL_STRING("XL @ Box", d[0].name);
    TEST_ASSERT_EQUAL_STRING("Neo @ Box", d[3].name);
}

/* A fast unplug / replug: the deck comes back to its own dock (its port in Companion), even dock 0,
 * and even when a new deck came meanwhile. */
static void test_pick_replug_returns_to_its_dock(void)
{
    const bool busy[6] = {false};
    const char *const map[6] = {"AB1CD2345678EF", "", "", "", "", ""};
    TEST_ASSERT_EQUAL_INT(0, dockproto_pick_dock(busy, map, 6, "AB1CD2345678EF", true));
    TEST_ASSERT_EQUAL_INT(1, dockproto_pick_dock(busy, map, 6, "NEW", true)); /* a newcomer keeps off */
    const bool busy2[6] = {false, true, false, false, false, false};          /* the newcomer on dock 1 */
    TEST_ASSERT_EQUAL_INT(0, dockproto_pick_dock(busy2, map, 6, "AB1CD2345678EF", true));
}

/* ---- dock allocation ---- */

/* A deck that comes back gets the dock it had (its Remote Surface entry in Companion is that port). */
static void test_pick_own_reserved_dock(void)
{
    const bool busy[6] = {false};
    const char *const map[6] = {"", "", "B", "", "", ""};
    TEST_ASSERT_EQUAL_INT(2, dockproto_pick_dock(busy, map, 6, "B", true));
}

/* A new deck takes an unreserved dock, not one an unplugged deck keeps (its owner would come back to
 * another port and Companion would stop driving it). */
static void test_pick_new_deck_prefers_unreserved(void)
{
    const bool busy[6] = {false};
    const char *const map[6] = {"A", "", "", "", "", ""};
    TEST_ASSERT_EQUAL_INT(1, dockproto_pick_dock(busy, map, 6, "NEW", true));
    const bool busy2[6] = {true, false, false, false, false, false};
    const char *const map2[6] = {"", "A", "C", "", NULL, ""};
    TEST_ASSERT_EQUAL_INT(3, dockproto_pick_dock(busy2, map2, 6, "NEW", true));
}

/* Only when every unreserved dock is taken: the lowest free reserved one. */
static void test_pick_reserved_only_when_all_taken(void)
{
    const bool busy[6] = {true, false, true, false, true, true};
    const char *const map[6] = {"", "A", "", "C", "", ""};
    TEST_ASSERT_EQUAL_INT(1, dockproto_pick_dock(busy, map, 6, "NEW", true));
}

/* the docks for the deck limit (2, or 4 unlocked), at most ELGDOCK_MAX's 6; a third deck at 2 finds no dock even
 * with docks 2..5 free (and a deck that once had dock 3 at 4 takes one of the two) */
static void test_dock_count_by_limit(void)
{
    TEST_ASSERT_EQUAL_INT(2, dockproto_ndocks(2, 6));
    TEST_ASSERT_EQUAL_INT(4, dockproto_ndocks(4, 6));
    TEST_ASSERT_EQUAL_INT(2, dockproto_ndocks(0, 6)); /* unset: the default */
    TEST_ASSERT_EQUAL_INT(6, dockproto_ndocks(9, 6));
    const bool busy[6] = {true, true, false, false, false, false};
    const char *const map[6] = {"A", "B", "", "C", "", ""};
    TEST_ASSERT_EQUAL_INT(-1, dockproto_pick_dock(busy, map, dockproto_ndocks(2, 6), "NEW", true));
    TEST_ASSERT_EQUAL_INT(2, dockproto_pick_dock(busy, map, dockproto_ndocks(4, 6), "NEW", true));
    const bool busy4[6] = {true, true, true, true, false, false};
    TEST_ASSERT_EQUAL_INT(-1, dockproto_pick_dock(busy4, map, dockproto_ndocks(4, 6), "NEW", true)); /* the 5th */
    const bool none[6] = {false};
    TEST_ASSERT_EQUAL_INT(0, dockproto_pick_dock(none, map, dockproto_ndocks(2, 6), "C", true)); /* its dock 3: gone */
}

static void test_pick_none_free(void)
{
    const bool busy[6] = {true, true, true, true, true, true};
    const char *const map[6] = {"", "", "", "", "", ""};
    TEST_ASSERT_EQUAL_INT(-1, dockproto_pick_dock(busy, map, 6, "NEW", true));
}

/* A stand-in serial ("unknown-<addr>") owns no reservation: it never matches one, and still prefers
 * unreserved docks. */
static void test_pick_non_sticky_serial(void)
{
    const bool busy[6] = {false};
    const char *const map[6] = {"unknown-3", "", "", "", "", ""};
    TEST_ASSERT_EQUAL_INT(1, dockproto_pick_dock(busy, map, 6, "unknown-3", false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_devinfo_vid_pid);
    RUN_TEST(test_no_child);
    RUN_TEST(test_serial_layout);
    RUN_TEST(test_mac_layout);
    RUN_TEST(test_keepalive_payload);
    RUN_TEST(test_vet);
    RUN_TEST(test_classify_ack);
    RUN_TEST(test_classify_host_get);
    RUN_TEST(test_classify_child_traffic);
    RUN_TEST(test_classify_other);
    RUN_TEST(test_pick_own_reserved_dock);
    RUN_TEST(test_pick_new_deck_prefers_unreserved);
    RUN_TEST(test_pick_reserved_only_when_all_taken);
    RUN_TEST(test_pick_none_free);
    RUN_TEST(test_pick_non_sticky_serial);
    RUN_TEST(test_classify_ack_needs_flag);
    RUN_TEST(test_deck_devinfo);
    RUN_TEST(test_deck_firmware_serial_mac);
    RUN_TEST(test_deck_encoder_firmware_absent);
    RUN_TEST(test_deck_no_child);
    RUN_TEST(test_deck_message_routes);
    RUN_TEST(test_deck_firmware_builder);
    RUN_TEST(test_mdns_name);
    RUN_TEST(test_mdns_name_limit);
    RUN_TEST(test_docks_wanted);
    RUN_TEST(test_docks_wanted_names_unique);
    RUN_TEST(test_dock_reconcile);
    RUN_TEST(test_reconcile_never_clashes);
    RUN_TEST(test_pick_replug_returns_to_its_dock);
    RUN_TEST(test_dock_count_by_limit);
    RUN_TEST(test_docks_wanted_matches_old);
    return UNITY_END();
}
