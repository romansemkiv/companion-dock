/* Settings password, sign-in sessions and the sign-in lockout: the pure rules behind auth.c
 * (no two-factor, API keys or setup window).
 *
 * Password: stored as PBKDF2-HMAC-SHA256 (AUTH_PBKDF2_ITERS, a random 16-byte salt), never as text.
 * Sessions: Auth.Login hands out a random token (32 bytes as 64 hex digits); only its SHA-256 is
 * kept, for AUTH_SESS_MS. Lockout: AUTH_LOCK_AFTER wrong passwords in a row from one client lock
 * that client for AUTH_LOCK_MS; afterwards it gets AUTH_LOCK_AFTER tries again, and each further lock is
 * twice as long, up to AUTH_LOCK_MAX_MS, until a right entry (a 4-digit PIN is short). Locked clients are
 * never forgotten to make room, and a box-wide budget slows a guesser that spreads over many
 * addresses.
 *
 * Times are milliseconds on any monotonic clock (the caller injects them). SHA-256 / PBKDF2 are
 * implemented here so the host tests check exactly the code the box runs. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_SALT_LEN 16
#define AUTH_DK_LEN 32
#define AUTH_PBKDF2_ITERS 10000
#define AUTH_TOKEN_BYTES 32
#define AUTH_TOKEN_HEX (AUTH_TOKEN_BYTES * 2)
#define AUTH_SESS_MS (12LL * 3600 * 1000)
#define AUTH_SESS_MAX 8
#define AUTH_LOCK_AFTER 5
#define AUTH_LOCK_MS 60000LL
#define AUTH_LIM_MAX 16
#define AUTH_CLIENT_LEN 48
#define AUTH_PASS_MIN 8
#define AUTH_PASS_MAX 64
#define AUTH_PIN_MIN 4 /* a new settings secret is a PIN of 4-8 digits */
#define AUTH_PIN_MAX 8
#define AUTH_LOCK_MAX_MS (3600LL * 1000) /* further locks of a client double, up to this */
#define AUTH_DECK_CLIENT "deck"          /* the decks' reserved client: never evicted, never held back */
/* Box-wide budget: after AUTH_BURST_FAILS wrong passwords (from anyone) within AUTH_BURST_WINDOW_MS,
 * at most one password check runs per AUTH_BURST_GAP_MS; the others are refused without hashing. */
#define AUTH_BURST_FAILS 5
#define AUTH_BURST_WINDOW_MS 10000LL
#define AUTH_BURST_GAP_MS 1000LL

/* ---- hashing ---- */

void auth_sha256(const void *data, size_t len, uint8_t out[32]);
void auth_pbkdf2_sha256(const char *pass, size_t pass_len, const uint8_t *salt, size_t salt_len, uint32_t iters,
                        uint8_t out[AUTH_DK_LEN]);
/* Compares without an early exit. */
bool auth_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

/* ---- the stored password verifier (an NVS blob; the layout is the storage format) ---- */

#define AUTH_PWV_VER 1
typedef struct {
    uint8_t ver;    /* AUTH_PWV_VER */
    uint8_t rsv[3];
    uint32_t iters;
    uint8_t salt[AUTH_SALT_LEN];
    uint8_t dk[AUTH_DK_LEN]; /* PBKDF2-HMAC-SHA256(password, salt, iters) */
} auth_pwv_t;

void auth_pwv_make(auth_pwv_t *pw, const char *pass, const uint8_t salt[AUTH_SALT_LEN], uint32_t iters);
/* A verifier as auth_pwv_make writes it (version, iterations in range). */
bool auth_pwv_valid(const auth_pwv_t *pw);
/* pass matches (false for an invalid verifier or a NULL / over-long pass). */
bool auth_pwv_check(const auth_pwv_t *pw, const char *pass);
/* AUTH_PASS_MIN..AUTH_PASS_MAX characters (the text passwords of older firmware: still checked, never set). */
bool auth_pass_ok(const char *pass);
/* AUTH_PIN_MIN..AUTH_PIN_MAX ASCII digits, nothing else. */
bool auth_pin_ok(const char *pin);
/* What the stored secret is: none, a text password of before (no PIN flag: the web sign-in takes
 * it, the deck can't be unlocked with it), or a PIN. */
typedef enum { AUTH_SECRET_NONE, AUTH_SECRET_PASSWORD, AUTH_SECRET_PIN } auth_secret_t;
auth_secret_t auth_secret_kind(bool verifier_stored, bool pin_flag);

/* A settings password stored as text by an older setup (boxcfg "adminPass"), at start. NONE: no text,
 * or its verifier isn't stored yet (keep the text until it is). SAVE: drop the text and save the
 * config. RAM_ONLY: drop it in RAM only, because boxconfig keeps a stored config that couldn't be
 * loaded whole and an automatic save would replace it; a deliberate save drops it later. */
typedef enum { AUTH_MIGRATE_NONE, AUTH_MIGRATE_RAM_ONLY, AUTH_MIGRATE_SAVE } auth_migrate_t;
auth_migrate_t auth_migrate_plan(bool have_text, bool verifier_stored, bool cfg_kept);

/* ---- sessions ---- */

/* 32 random bytes -> 64 lowercase hex digits + NUL. */
void auth_token_hex(const uint8_t raw[AUTH_TOKEN_BYTES], char out[AUTH_TOKEN_HEX + 1]);
/* SHA-256 of a well-formed token (64 hex digits); false otherwise. */
bool auth_token_hash(const char *token, uint8_t out[32]);

typedef struct {
    bool used;
    uint8_t hash[32]; /* SHA-256 of the token text */
    int64_t expires_ms;
    int64_t last_ms;
} auth_sess_t;

typedef struct {
    auth_sess_t s[AUTH_SESS_MAX];
} auth_sessions_t; /* zero-initialise */

/* Keeps the token's hash until now + AUTH_SESS_MS; a full table drops the least recently used. */
void auth_sess_add(auth_sessions_t *t, const char *token, int64_t now_ms);
/* A live session's token (refreshes its last use; drops expired sessions on the way). */
bool auth_sess_check(auth_sessions_t *t, const char *token, int64_t now_ms);
/* The same by the token's hash (auth_token_hash), for holders that keep no token text. */
bool auth_sess_check_hash(auth_sessions_t *t, const uint8_t hash[32], int64_t now_ms);
/* Ends that session; false when it was not found. */
bool auth_sess_remove(auth_sessions_t *t, const char *token);
/* The same by the token's hash (a WebSocket that signed in keeps only that). */
bool auth_sess_remove_hash(auth_sessions_t *t, const uint8_t hash[32]);
void auth_sess_clear(auth_sessions_t *t);
int auth_sess_count(const auth_sessions_t *t, int64_t now_ms);

/* ---- lockout, per client (its IP address) ---- */

typedef struct {
    char client[AUTH_CLIENT_LEN]; /* "" = free */
    uint8_t fails;                /* wrong passwords in a row since the last lock */
    uint8_t locks;                /* locks since the last right entry (each one longer) */
    int64_t until_ms;             /* locked until then */
    int64_t last_ms;
} auth_lim_entry_t;

typedef struct {
    auth_lim_entry_t e[AUTH_LIM_MAX];
    int64_t recent[AUTH_BURST_FAILS]; /* times of the latest wrong passwords, box-wide (a ring) */
    int n_recent, next;               /* entries in the ring; the oldest (next to overwrite) */
    int64_t last_check_ms;            /* the last password check auth_lim_begin let through */
    auth_lim_entry_t deck;            /* the decks' own entry (AUTH_DECK_CLIENT), never evicted */
    int64_t untracked_ms;             /* the last check of a newcomer the full table couldn't track */
} auth_lim_t; /* zero-initialise */

/* Seconds this client still has to wait (rounded up), 0 = it may try. The deck (AUTH_DECK_CLIENT) has its own
 * entry, never evicted or held back by the others. A client the table does not know while every entry
 * holds a locked client is not held back for that, only to one check a second among all such clients. */
int auth_lim_wait_s(const auth_lim_t *l, const char *client, int64_t now_ms);
/* Right before a password check: false with *wait_s (seconds, rounded up) when the client must
 * wait (auth_lim_wait_s) or the box-wide budget is spent; true lets this check run. The deck is outside the
 * box-wide budget: its own escalation bounds it, its checks and failures never spend the network's. */
bool auth_lim_begin(auth_lim_t *l, const char *client, int64_t now_ms, int *wait_s);
/* How long a client's lock lasts after `locks` earlier ones since its last right entry: AUTH_LOCK_MS,
 * doubling each time, at most AUTH_LOCK_MAX_MS. */
int64_t auth_lock_ms(int locks);
/* A wrong password: counts box-wide (not the deck's), and the AUTH_LOCK_AFTER-th in a row locks the client
 * for auth_lock_ms (returns true then). A full table reuses the unlocked entry idle the longest (one that
 * never locked first); when all are locked the failure is not tracked per client (a newcomer is then held to
 * one check a second). */
bool auth_lim_fail(auth_lim_t *l, const char *client, int64_t now_ms);
/* A right password: the client's count and its escalation start again. */
void auth_lim_ok(auth_lim_t *l, const char *client);
void auth_lim_clear(auth_lim_t *l);

/* zeroes n bytes at p as a wipe the compiler keeps (a memset of a buffer about to go out of scope is a dead store
 * it may drop): for secrets, a token or a password, in locals. */
void auth_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif
