#include <string.h>
#include "unity.h"
#include "cora.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t MAGIC[4] = {0x43, 0x93, 0x8a, 0x41};

static size_t make(uint8_t *out, size_t cap, uint16_t flags, uint8_t op, uint32_t id, const uint8_t *pl,
                   uint32_t len)
{
    const cora_msg_t m = {.flags = flags, .hid_op = op, .msg_id = id, .payload = pl, .len = len};
    return cora_encode(&m, out, cap);
}

static void test_magic_constant(void) { TEST_ASSERT_EQUAL_UINT8_ARRAY(MAGIC, cora_magic, 4); }

static void test_encode_header_layout(void)
{
    const uint8_t pl[3] = {0xaa, 0xbb, 0xcc};
    uint8_t out[32];
    memset(out, 0x5a, sizeof out);
    const size_t n = make(out, sizeof out, CORA_F_VERBATIM | CORA_F_RESULT, CORA_OP_GET_REPORT, 0x11223344, pl, 3);
    TEST_ASSERT_EQUAL_size_t(19, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(MAGIC, out, 4);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[4]); /* flags 0x8100 LE */
    TEST_ASSERT_EQUAL_HEX8(0x81, out[5]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[6]); /* hidOp */
    TEST_ASSERT_EQUAL_HEX8(0x00, out[7]);
    const uint8_t id[4] = {0x44, 0x33, 0x22, 0x11}, len[4] = {3, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(id, out + 8, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(len, out + 12, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pl, out + 16, 3);
}

static void test_encode_refuses_small_buffer(void)
{
    const uint8_t pl[3] = {1, 2, 3};
    uint8_t out[18];
    TEST_ASSERT_EQUAL_size_t(0, make(out, sizeof out, 0, 0, 0, pl, 3));
}

static void test_round_trip(void)
{
    uint8_t pl[300], wire[400], buf[512];
    for (int i = 0; i < 300; i++) pl[i] = (uint8_t)(i * 7);
    const size_t n = make(wire, sizeof wire, CORA_F_VERBATIM, CORA_OP_WRITE, 0xdeadbeef, pl, sizeof pl);
    TEST_ASSERT_EQUAL_size_t(316, n);

    cora_parser_t p;
    cora_parser_init(&p, buf, sizeof buf);
    cora_msg_t m;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(1, cora_feed(&p, wire, n, &used, &m));
    TEST_ASSERT_EQUAL_size_t(n, used);
    TEST_ASSERT_EQUAL_HEX16(CORA_F_VERBATIM, m.flags);
    TEST_ASSERT_EQUAL_HEX8(CORA_OP_WRITE, m.hid_op);
    TEST_ASSERT_EQUAL_HEX32(0xdeadbeef, m.msg_id);
    TEST_ASSERT_EQUAL_UINT32(300, m.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pl, m.payload, 300);
}

static void test_split_across_three_feeds(void)
{
    uint8_t pl[1024], wire[1100], buf[2048];
    for (int i = 0; i < 1024; i++) pl[i] = (uint8_t)(i ^ 0x5c);
    const size_t n = make(wire, sizeof wire, CORA_F_VERBATIM, CORA_OP_WRITE, 7, pl, sizeof pl);
    cora_parser_t p;
    cora_parser_init(&p, buf, sizeof buf);
    cora_msg_t m;
    size_t used = 0;
    /* in the middle of the header, then in the middle of the payload, then the rest */
    TEST_ASSERT_EQUAL_INT(0, cora_feed(&p, wire, 7, &used, &m));
    TEST_ASSERT_EQUAL_size_t(7, used);
    TEST_ASSERT_EQUAL_INT(0, cora_feed(&p, wire + 7, 500, &used, &m));
    TEST_ASSERT_EQUAL_size_t(500, used);
    TEST_ASSERT_EQUAL_INT(1, cora_feed(&p, wire + 507, n - 507, &used, &m));
    TEST_ASSERT_EQUAL_size_t(n - 507, used);
    TEST_ASSERT_EQUAL_UINT32(1024, m.len);
    TEST_ASSERT_EQUAL_UINT32(7, m.msg_id);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pl, m.payload, 1024);
}

static void test_two_messages_in_one_feed(void)
{
    const uint8_t a[2] = {0x03, 0x80}, b[1] = {0x08};
    uint8_t wire[64], buf[64];
    size_t n = make(wire, sizeof wire, CORA_F_NONE, CORA_OP_GET_REPORT, 1, a, 2);
    const size_t first = n;
    n += make(wire + n, sizeof wire - n, CORA_F_VERBATIM, CORA_OP_GET_REPORT, 2, b, 1);
    cora_parser_t p;
    cora_parser_init(&p, buf, sizeof buf);
    cora_msg_t m;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(1, cora_feed(&p, wire, n, &used, &m));
    TEST_ASSERT_EQUAL_size_t(first, used);
    TEST_ASSERT_EQUAL_UINT32(1, m.msg_id);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, m.payload, 2);
    TEST_ASSERT_EQUAL_INT(1, cora_feed(&p, wire + used, n - used, &used, &m));
    TEST_ASSERT_EQUAL_size_t(n - first, used);
    TEST_ASSERT_EQUAL_UINT32(2, m.msg_id);
    TEST_ASSERT_EQUAL_HEX16(CORA_F_VERBATIM, m.flags);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, m.payload, 1);
}

static void test_empty_payload(void)
{
    uint8_t wire[32], buf[16];
    const size_t n = make(wire, sizeof wire, CORA_F_RESULT, CORA_OP_GET_REPORT, 9, NULL, 0);
    TEST_ASSERT_EQUAL_size_t(16, n);
    cora_parser_t p;
    cora_parser_init(&p, buf, sizeof buf);
    cora_msg_t m;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(1, cora_feed(&p, wire, n, &used, &m));
    TEST_ASSERT_EQUAL_size_t(16, used);
    TEST_ASSERT_EQUAL_UINT32(0, m.len);
    TEST_ASSERT_EQUAL_UINT32(9, m.msg_id);
}

static void test_garbage_before_magic_skipped(void)
{
    /* noise, a broken magic, a magic start that repeats, then the real message */
    const uint8_t noise[] = {0x00, 0xff, 0x43, 0x93, 0x00, 0x43, 0x43, 0x93, 0x43};
    const uint8_t pl[4] = {1, 2, 3, 4};
    uint8_t wire[64], buf[64];
    memcpy(wire, noise, sizeof noise);
    const size_t n = sizeof noise + make(wire + sizeof noise, sizeof wire - sizeof noise, 0, CORA_OP_WRITE, 3, pl, 4);
    cora_parser_t p;
    cora_parser_init(&p, buf, sizeof buf);
    cora_msg_t m;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(1, cora_feed(&p, wire, n, &used, &m));
    TEST_ASSERT_EQUAL_size_t(n, used);
    TEST_ASSERT_EQUAL_UINT32(3, m.msg_id);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pl, m.payload, 4);
    TEST_ASSERT_EQUAL_UINT32(sizeof noise, p.resync);
}

static void test_oversize_payload_skipped_next_parses(void)
{
    uint8_t big[100], small[4] = {9, 8, 7, 6}, wire[256], buf[64];
    memset(big, 0x43, sizeof big); /* looks like magic starts: must be skipped by length, not scanned */
    size_t n = make(wire, sizeof wire, CORA_F_VERBATIM, CORA_OP_WRITE, 1, big, sizeof big);
    n += make(wire + n, sizeof wire - n, CORA_F_VERBATIM, CORA_OP_WRITE, 2, small, 4);
    cora_parser_t p;
    cora_parser_init(&p, buf, sizeof buf);
    cora_msg_t m;
    size_t used = 0;
    /* fed in two parts that split the oversize payload */
    TEST_ASSERT_EQUAL_INT(0, cora_feed(&p, wire, 60, &used, &m));
    TEST_ASSERT_EQUAL_size_t(60, used);
    TEST_ASSERT_EQUAL_INT(1, cora_feed(&p, wire + 60, n - 60, &used, &m));
    TEST_ASSERT_EQUAL_size_t(n - 60, used);
    TEST_ASSERT_EQUAL_UINT32(2, m.msg_id);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(small, m.payload, 4);
    TEST_ASSERT_EQUAL_UINT32(1, p.oversize);
}

static void test_payload_at_cap_taken_above_skipped(void)
{
    uint8_t pl[33], wire[128], buf[32];
    memset(pl, 0x11, sizeof pl);
    size_t n = make(wire, sizeof wire, 0, CORA_OP_WRITE, 1, pl, 32);          /* exactly cap */
    n += make(wire + n, sizeof wire - n, 0, CORA_OP_WRITE, 2, pl, 33);        /* cap + 1 */
    n += make(wire + n, sizeof wire - n, 0, CORA_OP_WRITE, 3, pl, 1);
    cora_parser_t p;
    cora_parser_init(&p, buf, sizeof buf);
    cora_msg_t m;
    size_t used = 0, at = 0;
    TEST_ASSERT_EQUAL_INT(1, cora_feed(&p, wire, n, &used, &m));
    TEST_ASSERT_EQUAL_UINT32(1, m.msg_id);
    TEST_ASSERT_EQUAL_UINT32(32, m.len);
    at += used;
    TEST_ASSERT_EQUAL_INT(1, cora_feed(&p, wire + at, n - at, &used, &m));
    TEST_ASSERT_EQUAL_UINT32(3, m.msg_id);
    TEST_ASSERT_EQUAL_UINT32(1, p.oversize);
}

static void test_magic_split_across_feeds(void)
{
    const uint8_t pl[2] = {0x03, 0x80};
    uint8_t wire[32], buf[16];
    const size_t n = make(wire, sizeof wire, 0, CORA_OP_GET_REPORT, 5, pl, 2);
    cora_parser_t p;
    cora_parser_init(&p, buf, sizeof buf);
    cora_msg_t m;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(0, cora_feed(&p, wire, 2, &used, &m)); /* 43 93 | 8a 41 ... */
    TEST_ASSERT_EQUAL_INT(1, cora_feed(&p, wire + 2, n - 2, &used, &m));
    TEST_ASSERT_EQUAL_UINT32(5, m.msg_id);
    TEST_ASSERT_EQUAL_UINT32(0, p.resync);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_magic_constant);
    RUN_TEST(test_encode_header_layout);
    RUN_TEST(test_encode_refuses_small_buffer);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_split_across_three_feeds);
    RUN_TEST(test_two_messages_in_one_feed);
    RUN_TEST(test_empty_payload);
    RUN_TEST(test_garbage_before_magic_skipped);
    RUN_TEST(test_oversize_payload_skipped_next_parses);
    RUN_TEST(test_payload_at_cap_taken_above_skipped);
    RUN_TEST(test_magic_split_across_feeds);
    return UNITY_END();
}
