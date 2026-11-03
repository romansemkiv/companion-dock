/* Pure rules behind auth.c (auth_logic.h): SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104),
 * PBKDF2-HMAC-SHA256 (RFC 8018), the session table and the per-client lockout. */
#include "auth_logic.h"

#include <stdio.h>
#include <string.h>

/* ---- SHA-256 ---- */

typedef struct {
    uint32_t h[8];
    uint64_t len; /* bytes hashed */
    uint8_t buf[64];
    size_t n;     /* bytes in buf */
} sha256_t;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(uint32_t h[8], const uint8_t b[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = bb;
        bb = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += bb;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

static void sha_init(sha256_t *s)
{
    static const uint32_t IV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, IV, sizeof IV);
    s->len = 0;
    s->n = 0;
}

static void sha_update(sha256_t *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->len += len;
    if (s->n) { /* top up a partial block first */
        const size_t k = 64 - s->n < len ? 64 - s->n : len;
        memcpy(s->buf + s->n, p, k);
        s->n += k;
        p += k;
        len -= k;
        if (s->n < 64) return;
        sha_block(s->h, s->buf);
        s->n = 0;
    }
    for (; len >= 64; p += 64, len -= 64) sha_block(s->h, p); /* whole blocks straight from the input */
    memcpy(s->buf, p, len);
    s->n = len;
}

static void put_be32(uint8_t *out, const uint32_t h[8])
{
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

static void sha_final(sha256_t *s, uint8_t out[32])
{
    const uint64_t bits = s->len * 8;
    s->buf[s->n++] = 0x80;
    if (s->n > 56) { /* no room for the length: one more block */
        memset(s->buf + s->n, 0, 64 - s->n);
        sha_block(s->h, s->buf);
        s->n = 0;
    }
    memset(s->buf + s->n, 0, 56 - s->n);
    for (int i = 0; i < 8; i++) s->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_block(s->h, s->buf);
    put_be32(out, s->h);
}

void auth_sha256(const void *data, size_t len, uint8_t out[32])
{
    sha256_t s;
    sha_init(&s);
    sha_update(&s, data, len);
    sha_final(&s, out);
}

/* ---- HMAC-SHA256 and PBKDF2 ---- */

/* A wipe the compiler keeps: a memset of a buffer about to go out of scope is a dead store it may drop. */
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

void auth_wipe(void *p, size_t n)
{
    if (p) wipe(p, n);
}

/* The key's inner and outer states, computed once per PBKDF2 run. */
typedef struct {
    sha256_t in, out;
} hmac_t;

static void hmac_init(hmac_t *m, const uint8_t *key, size_t klen)
{
    uint8_t k[64] = {0}, pad[64];
    if (klen > 64) auth_sha256(key, klen, k);
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha_init(&m->in);
    sha_update(&m->in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha_init(&m->out);
    sha_update(&m->out, pad, 64);
    wipe(k, sizeof k);
    wipe(pad, sizeof pad);
}

/* HMAC(key, a || b) from the prepared states. */
static void hmac_run(const hmac_t *m, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen,
                     uint8_t out[32])
{
    sha256_t s = m->in;
    uint8_t inner[32];
    sha_update(&s, a, alen);
    if (blen) sha_update(&s, b, blen);
    sha_final(&s, inner);
    s = m->out;
    sha_update(&s, inner, 32);
    sha_final(&s, out);
    wipe(inner, sizeof inner);
    wipe(&s, sizeof s);
}

void auth_pbkdf2_sha256(const char *pass, size_t pass_len, const uint8_t *salt, size_t salt_len, uint32_t iters,
                        uint8_t out[AUTH_DK_LEN])
{
    /* one output block (dkLen = hLen = 32): T1 = U1 ^ U2 ^ ... ^ Uc */
    hmac_t m;
    hmac_init(&m, (const uint8_t *)pass, pass_len);
    static const uint8_t block1[4] = {0, 0, 0, 1};
    uint8_t u[32];
    hmac_run(&m, salt, salt_len, block1, sizeof block1, u);
    memcpy(out, u, 32);
    /* U2..Uc: HMAC of 32 bytes. After the key's ipad/opad block, the inner and the outer hash each
     * take exactly one more block: the 32 bytes, 0x80, zeros, and the bit length 768 (64 + 32
     * bytes). The padding never changes, so each iteration is two block computations. */
    uint8_t blk[64] = {0};
    blk[32] = 0x80;
    blk[62] = 0x03; /* 768 = 0x0300 */
    uint32_t h[8];
    for (uint32_t i = 1; i < iters; i++) {
        memcpy(blk, u, 32);
        memcpy(h, m.in.h, sizeof h);
        sha_block(h, blk);
        put_be32(blk, h);
        memcpy(h, m.out.h, sizeof h);
        sha_block(h, blk);
        put_be32(u, h);
        for (int j = 0; j < 32; j++) out[j] ^= u[j];
    }
    wipe(u, sizeof u);
    wipe(blk, sizeof blk);
    wipe(h, sizeof h);
    wipe(&m, sizeof m);
}

bool auth_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* ---- password verifier ---- */

#define ITERS_MIN 1000
#define ITERS_MAX 1000000

void auth_pwv_make(auth_pwv_t *pw, const char *pass, const uint8_t salt[AUTH_SALT_LEN], uint32_t iters)
{
    memset(pw, 0, sizeof *pw);
    pw->ver = AUTH_PWV_VER;
    pw->iters = iters;
    memcpy(pw->salt, salt, AUTH_SALT_LEN);
    auth_pbkdf2_sha256(pass, strlen(pass), pw->salt, AUTH_SALT_LEN, iters, pw->dk);
}

bool auth_pwv_valid(const auth_pwv_t *pw)
{
    return pw && pw->ver == AUTH_PWV_VER && pw->iters >= ITERS_MIN && pw->iters <= ITERS_MAX;
}

bool auth_pin_ok(const char *pin)
{
    if (!pin) return false;
    size_t n = 0;
    for (; pin[n]; n++)
        if (pin[n] < '0' || pin[n] > '9' || n >= AUTH_PIN_MAX) return false;
    return n >= AUTH_PIN_MIN;
}

auth_secret_t auth_secret_kind(bool verifier_stored, bool pin_flag)
{
    return !verifier_stored ? AUTH_SECRET_NONE : pin_flag ? AUTH_SECRET_PIN : AUTH_SECRET_PASSWORD;
}

int64_t auth_lock_ms(int locks)
{
    int64_t ms = AUTH_LOCK_MS;
    for (int i = 0; i < locks && ms < AUTH_LOCK_MAX_MS; i++) ms *= 2;
    return ms < AUTH_LOCK_MAX_MS ? ms : AUTH_LOCK_MAX_MS;
}

bool auth_pass_ok(const char *pass)
{
    if (!pass) return false;
    const size_t n = strnlen(pass, AUTH_PASS_MAX + 1);
    return n >= AUTH_PASS_MIN && n <= AUTH_PASS_MAX;
}

bool auth_pwv_check(const auth_pwv_t *pw, const char *pass)
{
    if (!auth_pwv_valid(pw) || !pass) return false;
    const size_t n = strnlen(pass, AUTH_PASS_MAX + 1);
    if (n > AUTH_PASS_MAX) return false;
    uint8_t dk[AUTH_DK_LEN];
    auth_pbkdf2_sha256(pass, n, pw->salt, AUTH_SALT_LEN, pw->iters, dk);
    const bool ok = auth_ct_equal(dk, pw->dk, AUTH_DK_LEN);
    wipe(dk, sizeof dk);
    return ok;
}

/* ---- sessions ---- */

void auth_token_hex(const uint8_t raw[AUTH_TOKEN_BYTES], char out[AUTH_TOKEN_HEX + 1])
{
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < AUTH_TOKEN_BYTES; i++) {
        out[2 * i] = HEX[raw[i] >> 4];
        out[2 * i + 1] = HEX[raw[i] & 15];
    }
    out[AUTH_TOKEN_HEX] = '\0';
}

/* Only well-formed tokens are hashed and compared. */
bool auth_token_hash(const char *token, uint8_t out[32])
{
    if (!token || strnlen(token, AUTH_TOKEN_HEX + 1) != AUTH_TOKEN_HEX) return false;
    auth_sha256(token, AUTH_TOKEN_HEX, out);
    return true;
}

static bool sess_live(const auth_sess_t *s, int64_t now_ms) { return s->used && now_ms < s->expires_ms; }

void auth_sess_add(auth_sessions_t *t, const char *token, int64_t now_ms)
{
    uint8_t h[32];
    if (!auth_token_hash(token, h)) return;
    int slot = 0;
    for (int i = 0; i < AUTH_SESS_MAX; i++) { /* a free or expired slot, else the least recently used */
        if (!sess_live(&t->s[i], now_ms)) {
            slot = i;
            break;
        }
        if (t->s[i].last_ms < t->s[slot].last_ms) slot = i;
    }
    auth_sess_t *s = &t->s[slot];
    s->used = true;
    memcpy(s->hash, h, 32);
    s->expires_ms = now_ms + AUTH_SESS_MS;
    s->last_ms = now_ms;
}

bool auth_sess_check_hash(auth_sessions_t *t, const uint8_t hash[32], int64_t now_ms)
{
    bool ok = false;
    for (int i = 0; i < AUTH_SESS_MAX; i++) {
        auth_sess_t *s = &t->s[i];
        if (!s->used) continue;
        if (!sess_live(s, now_ms)) {
            memset(s, 0, sizeof *s);
            continue;
        }
        if (auth_ct_equal(s->hash, hash, 32)) {
            s->last_ms = now_ms;
            ok = true;
        }
    }
    return ok;
}

bool auth_sess_check(auth_sessions_t *t, const char *token, int64_t now_ms)
{
    uint8_t h[32];
    return auth_token_hash(token, h) && auth_sess_check_hash(t, h, now_ms);
}

bool auth_sess_remove_hash(auth_sessions_t *t, const uint8_t hash[32])
{
    if (!hash) return false;
    bool found = false;
    for (int i = 0; i < AUTH_SESS_MAX; i++)
        if (t->s[i].used && auth_ct_equal(t->s[i].hash, hash, 32)) {
            memset(&t->s[i], 0, sizeof t->s[i]);
            found = true;
        }
    return found;
}

bool auth_sess_remove(auth_sessions_t *t, const char *token)
{
    uint8_t h[32];
    return auth_token_hash(token, h) && auth_sess_remove_hash(t, h);
}

void auth_sess_clear(auth_sessions_t *t) { memset(t, 0, sizeof *t); }

int auth_sess_count(const auth_sessions_t *t, int64_t now_ms)
{
    int n = 0;
    for (int i = 0; i < AUTH_SESS_MAX; i++) n += sess_live(&t->s[i], now_ms);
    return n;
}

/* ---- lockout ---- */

/* The client's key as stored (cut to fit). */
static void client_key(const char *client, char out[AUTH_CLIENT_LEN])
{
    snprintf(out, AUTH_CLIENT_LEN, "%s", client && client[0] ? client : "?");
}

static bool is_deck(const char *key) { return !strcmp(key, AUTH_DECK_CLIENT); }

/* The client's entry; the deck's is its own, made on first use and never evicted. */
static auth_lim_entry_t *lim_find(auth_lim_t *l, const char *key)
{
    if (is_deck(key)) {
        if (!l->deck.client[0]) snprintf(l->deck.client, sizeof l->deck.client, "%s", AUTH_DECK_CLIENT);
        return &l->deck;
    }
    for (int i = 0; i < AUTH_LIM_MAX; i++)
        if (l->e[i].client[0] && !strcmp(l->e[i].client, key)) return &l->e[i];
    return NULL;
}

/* Every entry holds a locked client: a newcomer cannot be tracked. Locked entries are never evicted, or
 * cycling through more addresses than entries would escape the lockout; such a newcomer is not held
 * back for that, only to the box-wide budget (one check a second for all of them, untracked_until). */
static bool table_full_locked(const auth_lim_t *l, int64_t now_ms)
{
    for (int i = 0; i < AUTH_LIM_MAX; i++)
        if (!l->e[i].client[0] || l->e[i].until_ms <= now_ms) return false;
    return true;
}

static int wait_s(int64_t until_ms, int64_t now_ms)
{
    return until_ms > now_ms ? (int)((until_ms - now_ms + 999) / 1000) : 0;
}

/* An untracked newcomer (no entry, the table full of locked clients): when the next may check. */
static int64_t untracked_until(const auth_lim_t *l) { return l->untracked_ms ? l->untracked_ms + AUTH_BURST_GAP_MS : 0; }

int auth_lim_wait_s(const auth_lim_t *l, const char *client, int64_t now_ms)
{
    char key[AUTH_CLIENT_LEN];
    client_key(client, key);
    if (is_deck(key)) return wait_s(l->deck.until_ms, now_ms);
    const auth_lim_entry_t *e = lim_find((auth_lim_t *)l, key);
    if (e) return wait_s(e->until_ms, now_ms);
    return table_full_locked(l, now_ms) ? wait_s(untracked_until(l), now_ms) : 0;
}

/* AUTH_BURST_FAILS wrong passwords box-wide within AUTH_BURST_WINDOW_MS. */
static bool burst(const auth_lim_t *l, int64_t now_ms)
{
    return l->n_recent == AUTH_BURST_FAILS && now_ms - l->recent[l->next] < AUTH_BURST_WINDOW_MS;
}

bool auth_lim_begin(auth_lim_t *l, const char *client, int64_t now_ms, int *wait)
{
    char key[AUTH_CLIENT_LEN];
    client_key(client, key);
    const bool deck = is_deck(key); /* outside the box-wide budget: its own escalation bounds it */
    int w = auth_lim_wait_s(l, client, now_ms);
    if (!w && !deck && burst(l, now_ms) && now_ms < l->last_check_ms + AUTH_BURST_GAP_MS)
        w = wait_s(l->last_check_ms + AUTH_BURST_GAP_MS, now_ms);
    if (wait) *wait = w;
    if (w) return false;
    if (deck) return true; /* the network's next slot stays where it was */
    l->last_check_ms = now_ms;
    if (!lim_find(l, key) && table_full_locked(l, now_ms)) l->untracked_ms = now_ms;
    return true;
}

bool auth_lim_fail(auth_lim_t *l, const char *client, int64_t now_ms)
{
    char key[AUTH_CLIENT_LEN];
    client_key(client, key);
    if (!is_deck(key)) { /* box-wide, whoever it was on the network (the deck is outside the budget) */
        l->recent[l->next] = now_ms;
        l->next = (l->next + 1) % AUTH_BURST_FAILS;
        if (l->n_recent < AUTH_BURST_FAILS) l->n_recent++;
    }

    auth_lim_entry_t *e = lim_find(l, key);
    if (!e) {
        /* a free entry, else the unlocked one idle the longest; never a locked one */
        for (int i = 0; i < AUTH_LIM_MAX && !e; i++)
            if (!l->e[i].client[0]) e = &l->e[i];
        if (!e) { /* one that never locked first, so an escalation isn't lost to a crowd */
            for (int pass = 0; pass < 2 && !e; pass++)
                for (int i = 0; i < AUTH_LIM_MAX; i++) {
                    auth_lim_entry_t *c = &l->e[i];
                    if (c->until_ms > now_ms || (pass == 0 && c->locks)) continue;
                    if (!e || c->last_ms < e->last_ms) e = c;
                }
        }
        if (!e) return false; /* all locked: untracked, held to one check a second */
        memset(e, 0, sizeof *e);
        memcpy(e->client, key, sizeof e->client);
    }
    if (e->until_ms && e->until_ms <= now_ms) e->until_ms = 0; /* an ended lock: five tries again */
    e->last_ms = now_ms;
    if (++e->fails < AUTH_LOCK_AFTER) return false;
    e->fails = 0;
    e->until_ms = now_ms + auth_lock_ms(e->locks); /* 60 s, then twice as long each time, 1 h at most */
    if (e->locks < 255) e->locks++;
    return true;
}

void auth_lim_ok(auth_lim_t *l, const char *client)
{
    char key[AUTH_CLIENT_LEN];
    client_key(client, key);
    auth_lim_entry_t *e = lim_find(l, key);
    if (e) memset(e, 0, sizeof *e); /* the deck's entry is made again on its next use */
}

void auth_lim_clear(auth_lim_t *l) { memset(l, 0, sizeof *l); }

auth_migrate_t auth_migrate_plan(bool have_text, bool verifier_stored, bool cfg_kept)
{
    if (!have_text || !verifier_stored) return AUTH_MIGRATE_NONE;
    return cfg_kept ? AUTH_MIGRATE_RAM_ONLY : AUTH_MIGRATE_SAVE;
}
