#include <stdio.h>
#include <string.h>

#include "auth_logic.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* memmem is not standard C */
static const void *find(const void *hay, size_t n, const void *needle, size_t m)
{
    const unsigned char *h = hay;
    for (size_t i = 0; m && i + m <= n; i++)
        if (memcmp(h + i, needle, m) == 0) return h + i;
    return NULL;
}

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void assert_hex(const char *want, const uint8_t *d, size_t n)
{
    char got[129];
    hex(d, n, got);
    TEST_ASSERT_EQUAL_STRING(want, got);
}

/* Vectors from Python's hashlib (FIPS 180-2 / RFC 7914 inputs). */
static void test_sha256_vectors(void)
{
    uint8_t h[32];
    auth_sha256("", 0, h);
    assert_hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", h, 32);
    auth_sha256("abc", 3, h);
    assert_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", h, 32);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; /* 56 bytes: 2 blocks */
    auth_sha256(two, strlen(two), h);
    assert_hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", h, 32);
    char a[1000];
    memset(a, 'a', sizeof a);
    auth_sha256(a, sizeof a, h);
    assert_hex("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3", h, 32);
}

/* every padding case: the length fits in the last block or needs one more */
static void test_sha256_padding_boundaries(void)
{
    static const struct { int n; const char *hex; } v[] = {
        {55, "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072"},
        {56, "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e"},
        {63, "75220b47218278e656f2013bb8f0c455a25eaf01e86c64924e9d48d89776d6f2"},
        {64, "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c"},
        {65, "9537c5fdf120482f7d58d25e9ed583f52c02b4e304ea814db1633ad565aed7e9"},
        {119, "000b48d4edf0fa7bee3c6236ecd2785baa5db4eeb8bb54341b029e0d9fa5fb0c"},
        {120, "13f05a0b594787f5ecd315edc96141bd3243203d1b7d4f0836f37308b276ba98"},
    };
    char x[120];
    memset(x, 'x', sizeof x);
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t h[32];
        auth_sha256(x, (size_t)v[i].n, h);
        assert_hex(v[i].hex, h, 32);
    }
}

static void test_pbkdf2_vectors(void)
{
    uint8_t dk[32];
    auth_pbkdf2_sha256("password", 8, (const uint8_t *)"salt", 4, 1, dk);
    assert_hex("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b", dk, 32);
    auth_pbkdf2_sha256("password", 8, (const uint8_t *)"salt", 4, 4096, dk);
    assert_hex("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a", dk, 32);
    auth_pbkdf2_sha256("passwd", 6, (const uint8_t *)"salt", 4, 1, dk);
    assert_hex("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc", dk, 32);
    char k[100];
    memset(k, 'k', sizeof k); /* a key longer than the HMAC block is hashed first */
    auth_pbkdf2_sha256(k, sizeof k, (const uint8_t *)"NaCl", 4, 3, dk);
    assert_hex("c59d6ebf62a532411bd5d636667cdf91ccdf6ccb1049fc1e6df8601b0c8c8595", dk, 32);
}

static void test_pwv_make_and_verify(void)
{
    uint8_t salt[AUTH_SALT_LEN];
    for (int i = 0; i < AUTH_SALT_LEN; i++) salt[i] = (uint8_t)i;
    auth_pwv_t pw;
    auth_pwv_make(&pw, "correct horse", salt, AUTH_PBKDF2_ITERS);
    TEST_ASSERT_TRUE(auth_pwv_valid(&pw));
    TEST_ASSERT_EQUAL_UINT32(AUTH_PBKDF2_ITERS, pw.iters);
    TEST_ASSERT_EQUAL_MEMORY(salt, pw.salt, AUTH_SALT_LEN);
    /* the stored key is PBKDF2-HMAC-SHA256 with 10 000 iterations (Python hashlib) */
    assert_hex("e515070e0353a7db013961984b87babb9edccff4aadff4d081a0cfffc316e8bf", pw.dk, 32);
    TEST_ASSERT_NULL(find(&pw, sizeof pw, "correct horse", 13)); /* never the text */

    TEST_ASSERT_TRUE(auth_pwv_check(&pw, "correct horse"));
    TEST_ASSERT_FALSE(auth_pwv_check(&pw, "correct horsf"));
    TEST_ASSERT_FALSE(auth_pwv_check(&pw, "correct hors"));
    TEST_ASSERT_FALSE(auth_pwv_check(&pw, ""));
    TEST_ASSERT_FALSE(auth_pwv_check(&pw, NULL));

    auth_pwv_t bad = pw;
    bad.ver = 9;
    TEST_ASSERT_FALSE(auth_pwv_valid(&bad));
    TEST_ASSERT_FALSE(auth_pwv_check(&bad, "correct horse"));
    bad = pw;
    bad.iters = 0;
    TEST_ASSERT_FALSE(auth_pwv_check(&bad, "correct horse"));
    auth_pwv_t zero = {0};
    TEST_ASSERT_FALSE(auth_pwv_valid(&zero));
}

static void test_pass_rules(void)
{
    TEST_ASSERT_FALSE(auth_pass_ok(NULL));
    TEST_ASSERT_FALSE(auth_pass_ok("1234567"));
    TEST_ASSERT_TRUE(auth_pass_ok("12345678"));
    char p[AUTH_PASS_MAX + 2];
    memset(p, 'x', sizeof p);
    p[AUTH_PASS_MAX] = 0;
    TEST_ASSERT_TRUE(auth_pass_ok(p));
    p[AUTH_PASS_MAX] = 'x';
    p[AUTH_PASS_MAX + 1] = 0;
    TEST_ASSERT_FALSE(auth_pass_ok(p));
}

/* a new settings secret is a PIN of 4 to 8 digits, nothing else. */
static void test_pin_rules(void)
{
    TEST_ASSERT_FALSE(auth_pin_ok(NULL));
    TEST_ASSERT_FALSE(auth_pin_ok(""));
    TEST_ASSERT_FALSE(auth_pin_ok("123"));       /* 3 digits */
    TEST_ASSERT_TRUE(auth_pin_ok("1234"));       /* 4 */
    TEST_ASSERT_TRUE(auth_pin_ok("12345678"));   /* 8 */
    TEST_ASSERT_FALSE(auth_pin_ok("123456789")); /* 9 */
    TEST_ASSERT_FALSE(auth_pin_ok("12a4"));
    TEST_ASSERT_FALSE(auth_pin_ok(" 1234"));
    TEST_ASSERT_FALSE(auth_pin_ok("1234 "));
    TEST_ASSERT_FALSE(auth_pin_ok("12-34"));
    TEST_ASSERT_FALSE(auth_pin_ok("１２３４")); /* full-width digits: bytes, not ASCII digits */
}

/* a box that has a text password from before (no PIN flag) keeps working: the web sign-in checks it as
 * before, Auth.GetStatus says pin:false, and the deck can't be unlocked with it ("Set a PIN"). */
static void test_legacy_password_kind(void)
{
    TEST_ASSERT_EQUAL(AUTH_SECRET_NONE, auth_secret_kind(false, false));
    TEST_ASSERT_EQUAL(AUTH_SECRET_NONE, auth_secret_kind(false, true)); /* a stray flag, no verifier */
    TEST_ASSERT_EQUAL(AUTH_SECRET_PASSWORD, auth_secret_kind(true, false));
    TEST_ASSERT_EQUAL(AUTH_SECRET_PIN, auth_secret_kind(true, true));
    uint8_t salt[AUTH_SALT_LEN] = {7};
    auth_pwv_t pw;
    auth_pwv_make(&pw, "an old text password", salt, AUTH_PBKDF2_ITERS);
    TEST_ASSERT_TRUE(auth_pwv_check(&pw, "an old text password")); /* the sign-in still accepts it */
    TEST_ASSERT_FALSE(auth_pwv_check(&pw, "1234"));
    auth_pwv_make(&pw, "4711", salt, AUTH_PBKDF2_ITERS); /* a PIN is checked the same way */
    TEST_ASSERT_TRUE(auth_pwv_check(&pw, "4711"));
}

static void test_token_hex(void)
{
    uint8_t raw[AUTH_TOKEN_BYTES];
    for (int i = 0; i < AUTH_TOKEN_BYTES; i++) raw[i] = (uint8_t)(0xA0 + i);
    char t[AUTH_TOKEN_HEX + 1];
    auth_token_hex(raw, t);
    TEST_ASSERT_EQUAL_size_t(AUTH_TOKEN_HEX, strlen(t));
    TEST_ASSERT_EQUAL_STRING_LEN("a0a1a2a3", t, 8);
    TEST_ASSERT_EQUAL_STRING("bebf", t + 60);
}

static const char *TOK_A = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
static const char *TOK_B = "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";

static void test_session_token_hash_compare(void)
{
    auth_sessions_t t = {0};
    auth_sess_add(&t, TOK_A, 1000);
    TEST_ASSERT_TRUE(auth_sess_check(&t, TOK_A, 2000));
    TEST_ASSERT_FALSE(auth_sess_check(&t, TOK_B, 2000));
    TEST_ASSERT_FALSE(auth_sess_check(&t, "", 2000));
    TEST_ASSERT_FALSE(auth_sess_check(&t, NULL, 2000));
    char almost[AUTH_TOKEN_HEX + 1];
    strcpy(almost, TOK_A);
    almost[63] = 'e';
    TEST_ASSERT_FALSE(auth_sess_check(&t, almost, 2000));
    /* only the SHA-256 is kept: neither the text nor its bytes are in the table */
    TEST_ASSERT_NULL(find(&t, sizeof t, TOK_A, 16));
    uint8_t h[32];
    auth_sha256(TOK_A, strlen(TOK_A), h);
    TEST_ASSERT_NOT_NULL(find(&t, sizeof t, h, 32));
    TEST_ASSERT_EQUAL_INT(1, auth_sess_count(&t, 2000));
    /* by hash, for holders that keep no token text */
    TEST_ASSERT_TRUE(auth_token_hash(TOK_A, h));
    TEST_ASSERT_TRUE(auth_sess_check_hash(&t, h, 3000));
    TEST_ASSERT_TRUE(auth_token_hash(TOK_B, h));
    TEST_ASSERT_FALSE(auth_sess_check_hash(&t, h, 3000));
    TEST_ASSERT_FALSE(auth_token_hash("abc", h));
    /* logout by hash (a WebSocket keeps no token text) */
    TEST_ASSERT_TRUE(auth_token_hash(TOK_A, h));
    TEST_ASSERT_TRUE(auth_sess_remove_hash(&t, h));
    TEST_ASSERT_FALSE(auth_sess_remove_hash(&t, h));
    TEST_ASSERT_FALSE(auth_sess_check(&t, TOK_A, 3000));
    TEST_ASSERT_FALSE(auth_sess_remove_hash(&t, NULL));
    TEST_ASSERT_FALSE(auth_token_hash(NULL, h));
}

static void test_session_expiry_remove_clear(void)
{
    auth_sessions_t t = {0};
    auth_sess_add(&t, TOK_A, 0);
    TEST_ASSERT_TRUE(auth_sess_check(&t, TOK_A, AUTH_SESS_MS - 1));
    TEST_ASSERT_FALSE(auth_sess_check(&t, TOK_A, AUTH_SESS_MS)); /* 12 h after sign-in */
    TEST_ASSERT_EQUAL_INT(0, auth_sess_count(&t, AUTH_SESS_MS));

    auth_sess_add(&t, TOK_A, 0);
    auth_sess_add(&t, TOK_B, 0);
    TEST_ASSERT_TRUE(auth_sess_remove(&t, TOK_A));
    TEST_ASSERT_FALSE(auth_sess_remove(&t, TOK_A));
    TEST_ASSERT_FALSE(auth_sess_check(&t, TOK_A, 1));
    TEST_ASSERT_TRUE(auth_sess_check(&t, TOK_B, 1));
    auth_sess_clear(&t);
    TEST_ASSERT_FALSE(auth_sess_check(&t, TOK_B, 1));
}

static void test_session_full_evicts_least_recent(void)
{
    auth_sessions_t t = {0};
    char tok[AUTH_SESS_MAX + 1][AUTH_TOKEN_HEX + 1];
    for (int i = 0; i <= AUTH_SESS_MAX; i++) {
        memset(tok[i], 'a', AUTH_TOKEN_HEX);
        tok[i][AUTH_TOKEN_HEX] = 0;
        tok[i][0] = (char)('0' + i);
    }
    for (int i = 0; i < AUTH_SESS_MAX; i++) auth_sess_add(&t, tok[i], 100 + i);
    TEST_ASSERT_TRUE(auth_sess_check(&t, tok[0], 500)); /* 0 used recently: 1 is now the oldest */
    auth_sess_add(&t, tok[AUTH_SESS_MAX], 600);
    TEST_ASSERT_TRUE(auth_sess_check(&t, tok[0], 700));
    TEST_ASSERT_FALSE(auth_sess_check(&t, tok[1], 700));
    TEST_ASSERT_TRUE(auth_sess_check(&t, tok[AUTH_SESS_MAX], 700));
    TEST_ASSERT_EQUAL_INT(AUTH_SESS_MAX, auth_sess_count(&t, 700));
}

static void test_lockout_after_five_failures_released_after_60s(void)
{
    auth_lim_t l = {0};
    const char *ip = "192.168.1.20";
    int64_t now = 5000;
    for (int i = 1; i < AUTH_LOCK_AFTER; i++) {
        TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, ip, now));
        TEST_ASSERT_FALSE(auth_lim_fail(&l, ip, now));
        now += 100;
    }
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, ip, now));
    TEST_ASSERT_TRUE(auth_lim_fail(&l, ip, now)); /* the 5th */
    TEST_ASSERT_EQUAL_INT(60, auth_lim_wait_s(&l, ip, now));
    TEST_ASSERT_EQUAL_INT(30, auth_lim_wait_s(&l, ip, now + 30000));
    TEST_ASSERT_EQUAL_INT(1, auth_lim_wait_s(&l, ip, now + AUTH_LOCK_MS - 1));
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, "192.168.1.21", now)); /* other clients go on */
    /* released after 60 s, with five tries again; the next lock is twice as long */
    now += AUTH_LOCK_MS;
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, ip, now));
    for (int i = 1; i < AUTH_LOCK_AFTER; i++) TEST_ASSERT_FALSE(auth_lim_fail(&l, ip, now));
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, ip, now));
    TEST_ASSERT_TRUE(auth_lim_fail(&l, ip, now));
    TEST_ASSERT_EQUAL_INT(120, auth_lim_wait_s(&l, ip, now));
}

/* after the first 60 s lock, every further lock of that client doubles, up to 1 h; a right entry
 * resets it. The deck is one client ("deck") with the same rules; other clients are untouched. */
static void lock_once(auth_lim_t *l, const char *who, int64_t now)
{
    for (int i = 1; i < AUTH_LOCK_AFTER; i++) TEST_ASSERT_FALSE(auth_lim_fail(l, who, now));
    TEST_ASSERT_TRUE(auth_lim_fail(l, who, now));
}

static void test_lockout_escalates(void)
{
    TEST_ASSERT_EQUAL_INT64(60000, auth_lock_ms(0));
    TEST_ASSERT_EQUAL_INT64(120000, auth_lock_ms(1));
    TEST_ASSERT_EQUAL_INT64(240000, auth_lock_ms(2));
    TEST_ASSERT_EQUAL_INT64(1920000, auth_lock_ms(5));
    TEST_ASSERT_EQUAL_INT64(3600000, auth_lock_ms(6)); /* capped at 1 h */
    TEST_ASSERT_EQUAL_INT64(3600000, auth_lock_ms(40));
    auth_lim_t l = {0};
    int64_t now = 1000;
    static const int WAIT[] = {60, 120, 240, 480, 960, 1920, 3600, 3600};
    for (size_t k = 0; k < sizeof WAIT / sizeof WAIT[0]; k++) {
        lock_once(&l, "deck", now);
        TEST_ASSERT_EQUAL_INT(WAIT[k], auth_lim_wait_s(&l, "deck", now));
        TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, "192.168.1.30", now)); /* another client goes on */
        now += (int64_t)WAIT[k] * 1000;                                     /* it waits it out */
        TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, "deck", now));
    }
    auth_lim_ok(&l, "deck"); /* the right PIN: back to 60 s */
    lock_once(&l, "deck", now);
    TEST_ASSERT_EQUAL_INT(60, auth_lim_wait_s(&l, "deck", now));
    /* a web client's escalation is its own */
    lock_once(&l, "10.0.0.9", now);
    now += 60000;
    lock_once(&l, "10.0.0.9", now);
    TEST_ASSERT_EQUAL_INT(120, auth_lim_wait_s(&l, "10.0.0.9", now));
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, "deck", now)); /* the deck's 60 s ran out */
}

static void test_lockout_success_resets_count(void)
{
    auth_lim_t l = {0};
    const char *ip = "10.0.0.5";
    for (int i = 1; i < AUTH_LOCK_AFTER; i++) auth_lim_fail(&l, ip, 0);
    auth_lim_ok(&l, ip);
    for (int i = 1; i < AUTH_LOCK_AFTER; i++) TEST_ASSERT_FALSE(auth_lim_fail(&l, ip, 0));
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, ip, 0));
    auth_lim_clear(&l);
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, ip, 0));
}

static void test_lockout_table_full_keeps_locked_clients(void)
{
    auth_lim_t l = {0};
    char ip[AUTH_LIM_MAX + 1][24];
    for (int i = 0; i <= AUTH_LIM_MAX; i++) snprintf(ip[i], sizeof ip[i], "10.0.1.%d", i);
    for (int k = 0; k < AUTH_LOCK_AFTER; k++) auth_lim_fail(&l, ip[0], 0); /* locked */
    for (int i = 1; i < AUTH_LIM_MAX; i++) auth_lim_fail(&l, ip[i], 10 + i);
    auth_lim_fail(&l, ip[AUTH_LIM_MAX], 100);     /* table full: an unlocked entry makes room */
    TEST_ASSERT_EQUAL_INT(60, auth_lim_wait_s(&l, ip[0], 100));
    /* a long client address is cut, not overflowed */
    char longip[80];
    memset(longip, '9', sizeof longip - 1);
    longip[sizeof longip - 1] = 0;
    auth_lim_t m = {0};
    for (int k = 0; k < AUTH_LOCK_AFTER; k++) auth_lim_fail(&m, longip, 0);
    TEST_ASSERT_EQUAL_INT(60, auth_lim_wait_s(&m, longip, 0));
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&m, NULL, 0));
}

/* a guesser cycling through more addresses than the table holds stays locked out: locked clients are never
 * evicted. a newcomer to a table full of locked clients isn't blocked, it is held to the box-wide budget
 * (one try a second for all such newcomers), tracked best effort; its failures evict nobody. */
static void test_lockout_never_evicts_locked(void)
{
    auth_lim_t l = {0};
    char ip[AUTH_LIM_MAX + 2][24];
    for (int i = 0; i < AUTH_LIM_MAX + 2; i++) snprintf(ip[i], sizeof ip[i], "10.0.2.%d", i);
    for (int i = 0; i < AUTH_LIM_MAX; i++)
        for (int k = 0; k < AUTH_LOCK_AFTER; k++) auth_lim_fail(&l, ip[i], 1000);
    for (int i = 0; i < AUTH_LIM_MAX; i++) TEST_ASSERT_EQUAL_INT(60, auth_lim_wait_s(&l, ip[i], 1000));
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, ip[AUTH_LIM_MAX], 1000)); /* not blocked */
    int w = 0;
    TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[AUTH_LIM_MAX], 1000, &w));
    TEST_ASSERT_FALSE(auth_lim_fail(&l, ip[AUTH_LIM_MAX], 1100)); /* untracked: no entry to lock */
    TEST_ASSERT_FALSE(auth_lim_begin(&l, ip[AUTH_LIM_MAX + 1], 1500, &w)); /* another newcomer: the budget */
    TEST_ASSERT_EQUAL_INT(1, w);
    TEST_ASSERT_EQUAL_INT(1, auth_lim_wait_s(&l, ip[AUTH_LIM_MAX + 1], 1500));
    TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[AUTH_LIM_MAX + 1], 2000, &w)); /* a second on */
    TEST_ASSERT_FALSE(auth_lim_fail(&l, ip[AUTH_LIM_MAX + 1], 2000));
    for (int i = 0; i < AUTH_LIM_MAX; i++) TEST_ASSERT_TRUE(auth_lim_wait_s(&l, ip[i], 2000) > 0); /* none evicted */
}

/* the deck is a reserved client: never evicted, never held back by a full table. The scenario:
 * 16 network addresses escalated to 1 h locks fill the table; the deck still unlocks, and its own lockout
 * works as designed (its own escalating waits). */
static void test_lockout_deck_reserved(void)
{
    auth_lim_t l = {0};
    char ip[AUTH_LIM_MAX][24];
    int64_t now = 1000;
    for (int i = 0; i < AUTH_LIM_MAX; i++) snprintf(ip[i], sizeof ip[i], "10.0.4.%d", i);
    for (int round = 0; round < 7; round++) { /* 7 locks each: up to the 1 h cap */
        for (int i = 0; i < AUTH_LIM_MAX; i++)
            for (int k = 0; k < AUTH_LOCK_AFTER; k++) auth_lim_fail(&l, ip[i], now);
        now += 3600 * 1000LL;
    }
    for (int i = 0; i < AUTH_LIM_MAX; i++)
        for (int k = 0; k < AUTH_LOCK_AFTER; k++) auth_lim_fail(&l, ip[i], now);
    for (int i = 0; i < AUTH_LIM_MAX; i++) TEST_ASSERT_EQUAL_INT(3600, auth_lim_wait_s(&l, ip[i], now));
    /* the deck: not held back */
    int w = -1;
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, AUTH_DECK_CLIENT, now));
    TEST_ASSERT_TRUE(auth_lim_begin(&l, AUTH_DECK_CLIENT, now, &w));
    /* its own wrong entries lock it, escalating; a right PIN resets it; it is never evicted */
    for (int k = 1; k < AUTH_LOCK_AFTER; k++) TEST_ASSERT_FALSE(auth_lim_fail(&l, AUTH_DECK_CLIENT, now));
    TEST_ASSERT_TRUE(auth_lim_fail(&l, AUTH_DECK_CLIENT, now));
    TEST_ASSERT_EQUAL_INT(60, auth_lim_wait_s(&l, AUTH_DECK_CLIENT, now));
    now += 60000;
    for (int k = 0; k < AUTH_LOCK_AFTER; k++) auth_lim_fail(&l, AUTH_DECK_CLIENT, now);
    TEST_ASSERT_EQUAL_INT(120, auth_lim_wait_s(&l, AUTH_DECK_CLIENT, now));
    char other[24];
    for (int i = 0; i < 40; i++) { /* a crowd of other clients failing: the deck's entry stays */
        snprintf(other, sizeof other, "10.0.5.%d", i);
        auth_lim_fail(&l, other, now + 1000);
    }
    TEST_ASSERT_EQUAL_INT(119, auth_lim_wait_s(&l, AUTH_DECK_CLIENT, now + 1000));
    now += 120000;
    auth_lim_ok(&l, AUTH_DECK_CLIENT);
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, AUTH_DECK_CLIENT, now));
    for (int k = 0; k < AUTH_LOCK_AFTER; k++) auth_lim_fail(&l, AUTH_DECK_CLIENT, now);
    TEST_ASSERT_EQUAL_INT(60, auth_lim_wait_s(&l, AUTH_DECK_CLIENT, now)); /* reset to 60 s */
}

/* A full table makes room from an unlocked entry that never locked before one that did, so a crowd
 * can't wipe a client's escalation. */
static void test_lockout_evicts_never_locked_first(void)
{
    auth_lim_t l = {0};
    char ip[AUTH_LIM_MAX + 1][24];
    for (int i = 0; i <= AUTH_LIM_MAX; i++) snprintf(ip[i], sizeof ip[i], "10.0.6.%d", i);
    for (int k = 0; k < AUTH_LOCK_AFTER; k++) auth_lim_fail(&l, ip[0], 0); /* locked once, at 0 */
    for (int i = 1; i < AUTH_LIM_MAX; i++) auth_lim_fail(&l, ip[i], 100000 + i); /* newer, never locked */
    const int64_t t = 200000; /* ip[0]'s lock is over: unlocked, the idlest, but it escalated */
    auth_lim_fail(&l, ip[AUTH_LIM_MAX], t);   /* table full: a never-locked entry makes room, not ip[0] */
    for (int k = 0; k < AUTH_LOCK_AFTER; k++) auth_lim_fail(&l, ip[0], t);
    TEST_ASSERT_EQUAL_INT(120, auth_lim_wait_s(&l, ip[0], t)); /* its escalation was kept */
}

/* after a burst of wrong passwords box-wide, one check per second; the rest are not hashed */
static void test_box_wide_budget(void)
{
    auth_lim_t l = {0};
    char ip[10][24];
    for (int i = 0; i < 10; i++) snprintf(ip[i], sizeof ip[i], "10.0.3.%d", i);
    int64_t t = 0;
    int w = -1;
    for (int i = 0; i < AUTH_BURST_FAILS; i++, t += 100) { /* five addresses, one wrong guess each */
        TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[i], t, &w));
        TEST_ASSERT_EQUAL_INT(0, w);
        auth_lim_fail(&l, ip[i], t);
    }
    /* t = 500; the last check was at 400 */
    TEST_ASSERT_FALSE(auth_lim_begin(&l, ip[5], t, &w));
    TEST_ASSERT_EQUAL_INT(1, w);
    TEST_ASSERT_EQUAL_INT(0, auth_lim_wait_s(&l, ip[5], t)); /* its own record is clean */
    TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[5], 400 + AUTH_BURST_GAP_MS, &w));
    TEST_ASSERT_FALSE(auth_lim_begin(&l, ip[6], 500 + AUTH_BURST_GAP_MS, &w));
    /* a right password does not lift the budget; time does */
    auth_lim_ok(&l, ip[5]);
    TEST_ASSERT_FALSE(auth_lim_begin(&l, ip[7], 600 + AUTH_BURST_GAP_MS, &w));
    TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[7], AUTH_BURST_WINDOW_MS, &w)); /* the oldest failure (t=0) left the window */
    TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[8], AUTH_BURST_WINDOW_MS + 1, &w)); /* no gap needed */
    auth_lim_clear(&l);
    TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[9], 0, &w));
}

/* the deck is outside the box-wide budget (its own escalation bounds it): a burst from the
 * network never holds it back, and its own checks and failures never spend the network's budget. */
static void test_deck_outside_budget(void)
{
    auth_lim_t l = {0};
    char ip[8][24];
    for (int i = 0; i < 8; i++) snprintf(ip[i], sizeof ip[i], "10.0.4.%d", i);
    int64_t t = 0;
    int w = -1;
    for (int i = 0; i < AUTH_BURST_FAILS; i++, t += 100) {
        TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[i], t, &w));
        auth_lim_fail(&l, ip[i], t);
    }
    TEST_ASSERT_FALSE(auth_lim_begin(&l, ip[5], t, &w)); /* the network: one check a second now */
    TEST_ASSERT_TRUE(auth_lim_begin(&l, AUTH_DECK_CLIENT, t, &w)); /* the deck: at once */
    TEST_ASSERT_EQUAL_INT(0, w);
    auth_lim_fail(&l, AUTH_DECK_CLIENT, t);
    TEST_ASSERT_TRUE(auth_lim_begin(&l, AUTH_DECK_CLIENT, t + 10, &w)); /* and again: only the deck's own lockout bounds it */
    /* the deck's checks don't move the network's next slot */
    TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[5], 400 + AUTH_BURST_GAP_MS, &w));
    /* the deck's failures alone never make a burst */
    auth_lim_clear(&l);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(auth_lim_begin(&l, AUTH_DECK_CLIENT, i * 10, &w));
        auth_lim_fail(&l, AUTH_DECK_CLIENT, i * 10);
    }
    TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[0], 50, &w));
    auth_lim_fail(&l, ip[0], 50); /* 4 deck + 1 network failures: a burst only if the deck's were counted */
    TEST_ASSERT_TRUE(auth_lim_begin(&l, ip[1], 60, &w)); /* no burst: no gap */
}

/* the text password's migration never saves over a stored config that loading keeps. */
static void test_migrate_plan(void)
{
    TEST_ASSERT_EQUAL_INT(AUTH_MIGRATE_NONE, auth_migrate_plan(false, true, false));
    TEST_ASSERT_EQUAL_INT(AUTH_MIGRATE_NONE, auth_migrate_plan(true, false, false)); /* keep the text */
    TEST_ASSERT_EQUAL_INT(AUTH_MIGRATE_NONE, auth_migrate_plan(true, false, true));
    TEST_ASSERT_EQUAL_INT(AUTH_MIGRATE_SAVE, auth_migrate_plan(true, true, false));
    TEST_ASSERT_EQUAL_INT(AUTH_MIGRATE_RAM_ONLY, auth_migrate_plan(true, true, true));
}

/* the wipe the compiler keeps, for secrets in buffers about to go out of scope (auth.c's token and password) */
static void test_auth_wipe(void)
{
    char secret[24];
    strcpy(secret, "not a real token");
    auth_wipe(secret, sizeof secret);
    for (size_t i = 0; i < sizeof secret; i++) TEST_ASSERT_EQUAL_HEX8(0, (uint8_t)secret[i]);
    auth_wipe(NULL, 0); /* nothing to wipe */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sha256_vectors);
    RUN_TEST(test_sha256_padding_boundaries);
    RUN_TEST(test_pbkdf2_vectors);
    RUN_TEST(test_pwv_make_and_verify);
    RUN_TEST(test_pass_rules);
    RUN_TEST(test_pin_rules);
    RUN_TEST(test_legacy_password_kind);
    RUN_TEST(test_token_hex);
    RUN_TEST(test_session_token_hash_compare);
    RUN_TEST(test_session_expiry_remove_clear);
    RUN_TEST(test_session_full_evicts_least_recent);
    RUN_TEST(test_lockout_after_five_failures_released_after_60s);
    RUN_TEST(test_lockout_escalates);
    RUN_TEST(test_lockout_success_resets_count);
    RUN_TEST(test_lockout_table_full_keeps_locked_clients);
    RUN_TEST(test_lockout_never_evicts_locked);
    RUN_TEST(test_lockout_deck_reserved);
    RUN_TEST(test_lockout_evicts_never_locked_first);
    RUN_TEST(test_box_wide_budget);
    RUN_TEST(test_deck_outside_budget);
    RUN_TEST(test_migrate_plan);
    RUN_TEST(test_auth_wipe);
    return UNITY_END();
}
