#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "unit_logic.h"
#include "unity.h"

#define VECTOR_BOARD "80f1b2d68d1c"

static uint8_t vec[UNIT_SECTOR_SIZE]; /* test/vectors/unit-record-1.bin */
static uint8_t buf[UNIT_SECTOR_SIZE];
static unit_t u;

void setUp(void) { memset(&u, 0xA5, sizeof u); }
void tearDown(void) {}

static int load_vector(void)
{
    FILE *f = fopen(VECTOR_PATH, "rb");
    if (!f) return 0;
    size_t n = fread(vec, 1, sizeof vec, f);
    int more = fgetc(f);
    fclose(f);
    return n == UNIT_SECTOR_SIZE && more == EOF;
}

/* A sector holding payload (length len) under a correct header. */
static void make(const char *payload, size_t len)
{
    memset(buf, 0xFF, sizeof buf);
    memcpy(buf, "CDUNIT01", 8);
    buf[8] = (uint8_t)(len & 0xFF);
    buf[9] = (uint8_t)(len >> 8);
    uint32_t crc = unit_crc32((const uint8_t *)payload, len);
    for (int i = 0; i < 4; i++) buf[10 + i] = (uint8_t)(crc >> (8 * i));
    buf[14] = buf[15] = 0;
    if (len <= UNIT_SECTOR_SIZE - UNIT_HEADER_SIZE) memcpy(buf + UNIT_HEADER_SIZE, payload, len);
}

static unit_status_t parse_json(const char *json)
{
    make(json, strlen(json));
    return unit_parse(buf, sizeof buf, VECTOR_BOARD, &u);
}

static const unit_t zero;

static void test_crc32_is_zlibs(void)
{
    /* the CRC-32 check value: zlib.crc32(b"123456789") */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, unit_crc32((const uint8_t *)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0u, unit_crc32(NULL, 0));
    /* zlib.crc32 of the vector's payload (138 bytes), as Python works it out */
    TEST_ASSERT_EQUAL_UINT8(138, vec[8]);
    TEST_ASSERT_EQUAL_HEX32(0xB41CBCFCu, unit_crc32(vec + UNIT_HEADER_SIZE, 138));
}

static void test_vector_is_this_boards(void)
{
    TEST_ASSERT_EQUAL(UNIT_OK, unit_parse(vec, sizeof vec, VECTOR_BOARD, &u));
    TEST_ASSERT_EQUAL_STRING("CD-TEST-0001", u.case_sn);
    TEST_ASSERT_EQUAL_STRING("Standard", u.case_type);
    TEST_ASSERT_EQUAL_STRING("standard", u.edition);
    TEST_ASSERT_EQUAL_STRING("2026-11-19T12:00:00Z", u.at);
}

static void test_board_compared_in_either_case(void)
{
    TEST_ASSERT_EQUAL(UNIT_OK, unit_parse(vec, sizeof vec, "80F1B2D68D1C", &u));
}

static void test_vector_on_another_board(void)
{
    TEST_ASSERT_EQUAL(UNIT_OTHER_BOARD, unit_parse(vec, sizeof vec, "80f1b2d68d1d", &u));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &u, sizeof u);
    TEST_ASSERT_EQUAL(UNIT_OTHER_BOARD, unit_parse(vec, sizeof vec, "", &u));
    TEST_ASSERT_EQUAL(UNIT_OTHER_BOARD, unit_parse(vec, sizeof vec, "80f1b2d68d1c0", &u));
}

static void test_blank_sector_is_none(void)
{
    memset(buf, 0xFF, sizeof buf);
    TEST_ASSERT_EQUAL(UNIT_NONE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &u, sizeof u);
    memset(buf, 0x00, sizeof buf);
    TEST_ASSERT_EQUAL(UNIT_NONE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
}

static void test_wrong_magic_is_none(void)
{
    memcpy(buf, vec, sizeof buf);
    buf[7] = '2'; /* "CDUNIT02" */
    TEST_ASSERT_EQUAL(UNIT_NONE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
    memcpy(buf, vec, sizeof buf);
    buf[0] = 'c';
    TEST_ASSERT_EQUAL(UNIT_NONE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
    TEST_ASSERT_EQUAL(UNIT_NONE, unit_parse(vec, 7, VECTOR_BOARD, &u)); /* too short for a magic */
}

static void test_bad_crc_is_unreadable(void)
{
    memcpy(buf, vec, sizeof buf);
    buf[UNIT_HEADER_SIZE + 40] ^= 0x01; /* a payload bit */
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &u, sizeof u);
    memcpy(buf, vec, sizeof buf);
    buf[13] ^= 0x80; /* a CRC bit */
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
}

static void test_bad_length_is_unreadable(void)
{
    memcpy(buf, vec, sizeof buf);
    buf[8] = buf[9] = 0;
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
    memcpy(buf, vec, sizeof buf);
    buf[8] = 0x01; /* 1025 */
    buf[9] = 0x04;
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
    memcpy(buf, vec, sizeof buf);
    buf[8] = buf[9] = 0xFF;
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
    /* a buffer shorter than the header, or than the payload it announces */
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, unit_parse(vec, 15, VECTOR_BOARD, &u));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, unit_parse(vec, UNIT_HEADER_SIZE + 137, VECTOR_BOARD, &u));
    TEST_ASSERT_EQUAL(UNIT_OK, unit_parse(vec, UNIT_HEADER_SIZE + 138, VECTOR_BOARD, &u));
}

static void test_longest_payload(void)
{
    char json[UNIT_PAYLOAD_MAX + 2];
    const char *head = "{\"v\":1,\"board\":\"80f1b2d68d1c\",\"caseSn\":\"A\",\"caseType\":\"B\","
                       "\"edition\":\"standard\",\"at\":\"2026-11-19T12:00:00Z\",\"pad\":\"";
    size_t n = strlen(head);
    memcpy(json, head, n);
    while (n < UNIT_PAYLOAD_MAX - 2) json[n++] = 'x';
    memcpy(json + n, "\"}", 3);
    TEST_ASSERT_EQUAL_size_t(UNIT_PAYLOAD_MAX, strlen(json));
    TEST_ASSERT_EQUAL(UNIT_OK, parse_json(json));
    TEST_ASSERT_EQUAL_STRING("A", u.case_sn);
}

static void test_not_json_is_unreadable(void)
{
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json("hello"));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json("[1,2]"));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json("\"CD-TEST-0001\""));
    /* the JSON cut short, or with something after it */
    const char *good = (const char *)vec + UNIT_HEADER_SIZE;
    make(good, 137);
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
    char more[200];
    memcpy(more, good, 138);
    memcpy(more + 138, "{}", 2);
    make(more, 140);
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
    /* a NUL inside the payload */
    memcpy(more, good, 138);
    more[50] = '\0';
    make(more, 138);
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, unit_parse(buf, sizeof buf, VECTOR_BOARD, &u));
}

#define REC(fields) "{\"v\":1,\"board\":\"80f1b2d68d1c\"," fields "}"
#define ALL_BUT_SN "\"caseType\":\"Standard\",\"edition\":\"standard\",\"at\":\"2026-11-19T12:00:00Z\""

static void test_missing_fields_are_unreadable(void)
{
    TEST_ASSERT_EQUAL(UNIT_OK, parse_json(REC("\"caseSn\":\"CD-TEST-0001\"," ALL_BUT_SN)));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(REC(ALL_BUT_SN)));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(REC("\"caseSn\":\"CD-TEST-0001\",\"edition\":\"standard\","
                                                      "\"at\":\"2026-11-19T12:00:00Z\"")));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(REC("\"caseSn\":\"CD-TEST-0001\",\"caseType\":\"Standard\","
                                                      "\"at\":\"2026-11-19T12:00:00Z\"")));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(REC("\"caseSn\":\"CD-TEST-0001\",\"caseType\":\"Standard\","
                                                      "\"edition\":\"standard\"")));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE,
                      parse_json("{\"v\":1,\"caseSn\":\"CD-TEST-0001\"," ALL_BUT_SN "}")); /* no board */
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE,
                      parse_json("{\"board\":\"80f1b2d68d1c\",\"caseSn\":\"CD-TEST-0001\"," ALL_BUT_SN "}"));
}

static void test_wrong_types_and_values_are_unreadable(void)
{
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(REC("\"caseSn\":1," ALL_BUT_SN)));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(REC("\"caseSn\":null," ALL_BUT_SN)));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(REC("\"caseSn\":\"\"," ALL_BUT_SN)));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(REC("\"caseSn\":\"CD\\u00e9\"," ALL_BUT_SN))); /* not ASCII */
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(REC("\"caseSn\":\"CD\\n1\"," ALL_BUT_SN)));     /* control */
    /* another format version; a board that isn't 12 hex digits */
#define VB(v, board) "{\"v\":" v ",\"board\":" board ",\"caseSn\":\"A\"," ALL_BUT_SN "}"
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(VB("2", "\"80f1b2d68d1c\"")));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(VB("\"1\"", "\"80f1b2d68d1c\"")));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(VB("1.5", "\"80f1b2d68d1c\"")));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(VB("1", "\"80f1b2d68d1\"")));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(VB("1", "\"80f1b2d68d1g\"")));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(VB("1", "\"80:f1:b2:d6:8d:1c\"")));
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(VB("1", "12")));
}

static void test_too_long_is_refused_not_cut(void)
{
    /* 32 characters fit; 33 make the record unreadable (never a shortened case SN) */
    TEST_ASSERT_EQUAL(UNIT_OK, parse_json(REC("\"caseSn\":\"0123456789abcdef0123456789ABCDEF\"," ALL_BUT_SN)));
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789ABCDEF", u.case_sn);
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(REC("\"caseSn\":\"0123456789abcdef0123456789ABCDEFX\"," ALL_BUT_SN)));
    TEST_ASSERT_EQUAL_MEMORY(&zero, &u, sizeof u);
#define TYPE_FMT REC("%s\"caseType\":\"%s\",\"edition\":\"standard\",\"at\":\"2026-11-19T12:00:00Z\"")
    const char *sn = "\"caseSn\":\"A\",";
    char json[400];
    snprintf(json, sizeof json, TYPE_FMT, sn, "123456789012345678901234");
    TEST_ASSERT_EQUAL(UNIT_OK, parse_json(json));
    TEST_ASSERT_EQUAL_STRING("123456789012345678901234", u.case_type);
    snprintf(json, sizeof json, TYPE_FMT, sn, "1234567890123456789012345");
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(json));
    snprintf(json, sizeof json, REC("%s\"caseType\":\"B\",\"edition\":\"%s\",\"at\":\"2026-11-19T12:00:00Z\""), sn,
             "standard2");
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(json));
    snprintf(json, sizeof json, REC("%s\"caseType\":\"B\",\"edition\":\"standard\",\"at\":\"%s\""), sn,
             "2026-11-19T12:00:00.000Z");
    TEST_ASSERT_EQUAL(UNIT_OK, parse_json(json));
    TEST_ASSERT_EQUAL_STRING("2026-11-19T12:00:00.000Z", u.at);
    snprintf(json, sizeof json, REC("%s\"caseType\":\"B\",\"edition\":\"standard\",\"at\":\"%s\""), sn,
             "2026-11-19T12:00:00.0000Z");
    TEST_ASSERT_EQUAL(UNIT_UNREADABLE, parse_json(json));
}

static void test_other_keys_ignored(void)
{
    TEST_ASSERT_EQUAL(UNIT_OK, parse_json(REC("\"caseSn\":\"CD-TEST-0001\"," ALL_BUT_SN
                                              ",\"fw\":{\"odd\":[1]},\"later\":true")));
    TEST_ASSERT_EQUAL_STRING("CD-TEST-0001", u.case_sn);
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *txt = malloc((size_t)n + 1);
    TEST_ASSERT_NOT_NULL(txt);
    TEST_ASSERT_EQUAL(n, (long)fread(txt, 1, (size_t)n, f));
    txt[n] = '\0';
    fclose(f);
    return txt;
}

static int nibble(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

/* test/vectors/unit-payloads.json: each payload under a correct header, read as the case says (the encoder's
 * test reads the same cases) */
static void test_shared_payloads(void)
{
    char *txt = read_text(PAYLOADS_PATH);
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_STRING(VECTOR_BOARD, cJSON_GetObjectItem(root, "board")->valuestring);
    const cJSON *cases = cJSON_GetObjectItem(root, "cases");
    TEST_ASSERT_TRUE(cJSON_GetArraySize(cases) >= 30);
    static char payload[UNIT_PAYLOAD_MAX];
    const cJSON *c;
    cJSON_ArrayForEach(c, cases)
    {
        const char *name = cJSON_GetObjectItem(c, "name")->valuestring;
        const char *hex = cJSON_GetObjectItem(c, "hex")->valuestring;
        const size_t n = strlen(hex) / 2;
        TEST_ASSERT_TRUE_MESSAGE(n >= 1 && n <= UNIT_PAYLOAD_MAX, name);
        for (size_t i = 0; i < n; i++) payload[i] = (char)(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
        make(payload, n);
        const unit_status_t st = unit_parse(buf, sizeof buf, VECTOR_BOARD, &u);
        if (strcmp(cJSON_GetObjectItem(c, "status")->valuestring, "ok") == 0) {
            TEST_ASSERT_EQUAL_MESSAGE(UNIT_OK, st, name);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(cJSON_GetObjectItem(c, "caseSn")->valuestring, u.case_sn, name);
        } else {
            TEST_ASSERT_EQUAL_MESSAGE(UNIT_UNREADABLE, st, name);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&zero, &u, sizeof u, name);
        }
    }
    cJSON_Delete(root);
}

int main(void)
{
    if (!load_vector()) {
        fprintf(stderr, "cannot read the 4096-byte vector " VECTOR_PATH "\n");
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_crc32_is_zlibs);
    RUN_TEST(test_vector_is_this_boards);
    RUN_TEST(test_board_compared_in_either_case);
    RUN_TEST(test_vector_on_another_board);
    RUN_TEST(test_blank_sector_is_none);
    RUN_TEST(test_wrong_magic_is_none);
    RUN_TEST(test_bad_crc_is_unreadable);
    RUN_TEST(test_bad_length_is_unreadable);
    RUN_TEST(test_longest_payload);
    RUN_TEST(test_not_json_is_unreadable);
    RUN_TEST(test_missing_fields_are_unreadable);
    RUN_TEST(test_wrong_types_and_values_are_unreadable);
    RUN_TEST(test_too_long_is_refused_not_cut);
    RUN_TEST(test_other_keys_ignored);
    RUN_TEST(test_shared_payloads);
    return UNITY_END();
}
