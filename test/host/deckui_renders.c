/* Host tool: PNG renders of the deck maintenance UI (components/deckui) on every model family, one file
 * per screen and page, composed as the whole deck (keys with gaps, the strip or the info bar), and a
 * contact sheet per model. Also prints the QR module sizes of the hotspot join string per key size.
 *   build-host/deckui_renders <out dir>           the reference screens (deckui_fixtures)
 *   build-host/deckui_renders <out dir> screens   the real builders (deckui/screens.h) on a few snapshots
 *   build-host/deckui_renders <out dir> hints     every hold button let go early, on every key size
 *   build-host/deckui_renders <out dir> pin       the PIN pad (deckui/pin.h) in each of its states
 *   build-host/deckui_renders <out dir> icons     every icon: as buttons on 72, 96 and 120 px keys, and alone at
 *                                                  72, 96 and 120 px
 *   build-host/deckui_renders <out dir> ipedit    the static-address editor (deckui/ipedit.h) and its host
 *                                                  mode, the Companion's IP
 * Not a test (ctest doesn't run it); tools/test-host.sh builds it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../../components/deckui/dui_int.h"
#include "../../components/deckui/qrcodegen/qrcodegen.h"
#include "deckmodel/model.h"
#include "deckui/deckui.h"
#include "deckui/pin.h"
#include "deckui/ipedit.h"
#include "deckui/screens.h"
#include "deckui_fixtures.h"

#define GAP 14     /* px between keys */
#define PAD 22     /* the deck's body round the keys */
#define BODY 0x050607
#define SHEET_BG 0x202226

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t hdr[8];
    be32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uLong crc = crc32(0L, hdr + 4, 4);
    crc = crc32(crc, data, n);
    uint8_t tail[4];
    be32(tail, (uint32_t)crc);
    fwrite(tail, 1, 4, f);
}

static int write_png(const char *path, int w, int h, const uint8_t *rgb)
{
    const size_t row = 1 + (size_t)w * 3, raw_len = row * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    uLongf z_len = compressBound((uLong)raw_len);
    uint8_t *z = malloc(z_len);
    if (!raw || !z) return -1;
    for (int y = 0; y < h; y++) {
        raw[row * (size_t)y] = 0;
        memcpy(raw + row * (size_t)y + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    int rc = compress2(z, &z_len, raw, (uLong)raw_len, 6) == Z_OK ? 0 : -1;
    FILE *f = rc == 0 ? fopen(path, "wb") : NULL;
    if (f) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        uint8_t ihdr[13];
        be32(ihdr, (uint32_t)w);
        be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;
        ihdr[9] = 2;
        ihdr[10] = ihdr[11] = ihdr[12] = 0;
        fwrite(sig, 1, 8, f);
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, (uint32_t)z_len);
        chunk(f, "IEND", NULL, 0);
        rc = fclose(f) == 0 ? 0 : -1;
    } else {
        rc = -1;
    }
    free(raw);
    free(z);
    return rc;
}

typedef struct {
    uint8_t *rgb;
    int w, h;
} image_t;

static image_t image(int w, int h, uint32_t bg)
{
    image_t im = {malloc((size_t)w * h * 3), w, h};
    if (!im.rgb) exit(1);
    for (int i = 0; i < w * h; i++) {
        im.rgb[3 * i] = (uint8_t)(bg >> 16);
        im.rgb[3 * i + 1] = (uint8_t)(bg >> 8);
        im.rgb[3 * i + 2] = (uint8_t)bg;
    }
    return im;
}

/* src (sw x sh) into dst at (x, y), its corners rounded by radius rad (0: square). */
static void blit(image_t *dst, const uint8_t *src, int sw, int sh, int x, int y, int rad)
{
    for (int j = 0; j < sh; j++)
        for (int i = 0; i < sw; i++) {
            if (rad) { /* outside the rounded corner: the body shows */
                const int cx = i < rad ? rad - i : i >= sw - rad ? i - (sw - rad - 1) : 0;
                const int cy = j < rad ? rad - j : j >= sh - rad ? j - (sh - rad - 1) : 0;
                if (cx && cy && cx * cx + cy * cy > rad * rad) continue;
            }
            const int X = x + i, Y = y + j;
            if (X < 0 || Y < 0 || X >= dst->w || Y >= dst->h) continue;
            memcpy(dst->rgb + 3 * ((size_t)Y * dst->w + X), src + 3 * ((size_t)j * sw + i), 3);
        }
}

/* src scaled to dw x dh (area average) into dst at (x, y). */
static void blit_scaled(image_t *dst, const uint8_t *src, int sw, int sh, int x, int y, int dw, int dh)
{
    for (int j = 0; j < dh; j++)
        for (int i = 0; i < dw; i++) {
            const int x0 = i * sw / dw, x1 = (i + 1) * sw / dw > x0 ? (i + 1) * sw / dw : x0 + 1;
            const int y0 = j * sh / dh, y1 = (j + 1) * sh / dh > y0 ? (j + 1) * sh / dh : y0 + 1;
            unsigned s[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++, n++)
                    for (int k = 0; k < 3; k++) s[k] += src[3 * ((size_t)yy * sw + xx) + k];
            const int X = x + i, Y = y + j;
            if (!n || X < 0 || Y < 0 || X >= dst->w || Y >= dst->h) continue;
            for (int k = 0; k < 3; k++) dst->rgb[3 * ((size_t)Y * dst->w + X) + k] = (uint8_t)(s[k] / n);
        }
}

static void disc(image_t *im, float cx, float cy, float r, uint32_t rgb, bool ring)
{
    const dui_canvas_t c = {im->rgb, im->w, im->h};
    for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 2); y++)
        for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 2); x++) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d2 = dx * dx + dy * dy;
            const bool in = ring ? d2 <= r * r && d2 >= (r - 2) * (r - 2) : d2 <= r * r;
            if (in && x >= 0 && y >= 0 && x < c.w && y < c.h) {
                uint8_t *o = im->rgb + 3 * ((size_t)y * im->w + x);
                o[0] = (uint8_t)(rgb >> 16);
                o[1] = (uint8_t)(rgb >> 8);
                o[2] = (uint8_t)rgb;
            }
        }
}

/* Page `page` of screen s on deck d (laid out as L), as the deck shows it. */
static image_t deck_page(const dui_deck_t *d, const dui_screen_t *s, const dui_layout_t *L, int page)
{
    const int px = d->key_px, gw = d->cols * px + (d->cols - 1) * GAP, gh = d->rows * px + (d->rows - 1) * GAP;
    int bar_h = 0;
    if (d->bar == DUI_BAR_STRIP) bar_h = d->bar_h * gw / d->bar_w;
    if (d->bar == DUI_BAR_INFO) bar_h = d->bar_h;
    const int extra = d->bar == DUI_BAR_STRIP ? GAP + bar_h + GAP + 60 : d->bar == DUI_BAR_INFO ? GAP + bar_h : 0;
    image_t im = image(gw + 2 * PAD, gh + 2 * PAD + extra, BODY);
    uint8_t *key = malloc((size_t)px * px * 3);
    for (int r = 0; r < d->rows; r++)
        for (int c = 0; c < d->cols; c++) {
            dui_tile_t t;
            dui_layout_key(L, page, c, r, &t);
            dui_render_key(&t, px, key);
            blit(&im, key, px, px, PAD + c * (px + GAP), PAD + r * (px + GAP), px * 12 / 100);
        }
    free(key);
    if (d->bar != DUI_BAR_NONE) {
        uint8_t *bar = malloc((size_t)d->bar_w * d->bar_h * 3);
        dui_render_bar(&s->status, s->sub, page, L->pages, d->bar_w, d->bar_h, bar);
        const int y = PAD + gh + GAP;
        if (d->bar == DUI_BAR_STRIP) {
            blit_scaled(&im, bar, d->bar_w, d->bar_h, PAD, y, gw, bar_h);
            for (int i = 0; i < d->dials; i++) /* the dials under the strip */
                disc(&im, PAD + gw * (2 * i + 1) / (2.0f * d->dials), (float)(y + bar_h + GAP + 28), 26, 0x2A2F37, false);
        } else { /* the info bar, the touch keys either side (lit when there are pages) */
            const int x = PAD + (gw - d->bar_w) / 2;
            blit(&im, bar, d->bar_w, d->bar_h, x, y, 6);
            const uint32_t ring = L->pages > 1 ? SUI_RGB_ACCENT : 0x333A44;
            disc(&im, (float)(x - 30), y + d->bar_h / 2.0f, 16, ring, true);
            disc(&im, (float)(x + d->bar_w + 30), y + d->bar_h / 2.0f, 16, ring, true);
        }
        free(bar);
    }
    return im;
}

/* The PIN pad's page: its keys, and its words and dots on the strip or the info bar. */
static image_t pin_page(const dui_deck_t *d, const dui_pin_t *p, const dui_layout_t *L, int page, int64_t now)
{
    const int px = d->key_px, gw = d->cols * px + (d->cols - 1) * GAP, gh = d->rows * px + (d->rows - 1) * GAP;
    int bar_h = 0;
    if (d->bar == DUI_BAR_STRIP) bar_h = d->bar_h * gw / d->bar_w;
    if (d->bar == DUI_BAR_INFO) bar_h = d->bar_h;
    const int extra = d->bar == DUI_BAR_STRIP ? GAP + bar_h + GAP + 60 : d->bar == DUI_BAR_INFO ? GAP + bar_h : 0;
    image_t im = image(gw + 2 * PAD, gh + 2 * PAD + extra, BODY);
    uint8_t *key = malloc((size_t)px * px * 3);
    for (int r = 0; r < d->rows; r++)
        for (int c = 0; c < d->cols; c++) {
            dui_tile_t t;
            dui_layout_key(L, page, c, r, &t);
            dui_pin_decorate(p, c, r, &t);
            dui_render_key(&t, px, key);
            blit(&im, key, px, px, PAD + c * (px + GAP), PAD + r * (px + GAP), px * 12 / 100);
        }
    free(key);
    if (d->bar != DUI_BAR_NONE) {
        uint8_t *bar = malloc((size_t)d->bar_w * d->bar_h * 3);
        dui_render_pin_bar(p, now, d->bar_w, d->bar_h, bar);
        const int y = PAD + gh + GAP;
        if (d->bar == DUI_BAR_STRIP) {
            blit_scaled(&im, bar, d->bar_w, d->bar_h, PAD, y, gw, bar_h);
            for (int i = 0; i < d->dials; i++)
                disc(&im, PAD + gw * (2 * i + 1) / (2.0f * d->dials), (float)(y + bar_h + GAP + 28), 26, 0x2A2F37, false);
        } else {
            const int x = PAD + (gw - d->bar_w) / 2;
            blit(&im, bar, d->bar_w, d->bar_h, x, y, 6);
            disc(&im, (float)(x - 30), y + d->bar_h / 2.0f, 16, SUI_RGB_ACCENT, true);
            disc(&im, (float)(x + d->bar_w + 30), y + d->bar_h / 2.0f, 16, SUI_RGB_ACCENT, true);
        }
        free(bar);
    }
    return im;
}

/* The static-address editor's page: its keys, and its field and value on the strip or the info bar. */
static image_t ipe_page(const dui_deck_t *d, const dui_ipe_t *e, const dui_layout_t *L, int page, int64_t now)
{
    const int px = d->key_px, gw = d->cols * px + (d->cols - 1) * GAP, gh = d->rows * px + (d->rows - 1) * GAP;
    int bar_h = 0;
    if (d->bar == DUI_BAR_STRIP) bar_h = d->bar_h * gw / d->bar_w;
    if (d->bar == DUI_BAR_INFO) bar_h = d->bar_h;
    const int extra = d->bar == DUI_BAR_STRIP ? GAP + bar_h + GAP + 60 : d->bar == DUI_BAR_INFO ? GAP + bar_h : 0;
    image_t im = image(gw + 2 * PAD, gh + 2 * PAD + extra, BODY);
    uint8_t *key = malloc((size_t)px * px * 3);
    for (int r = 0; r < d->rows; r++)
        for (int c = 0; c < d->cols; c++) {
            dui_tile_t t;
            dui_layout_key(L, page, c, r, &t);
            dui_ipe_decorate(e, c, r, now, &t);
            dui_render_key(&t, px, key);
            blit(&im, key, px, px, PAD + c * (px + GAP), PAD + r * (px + GAP), px * 12 / 100);
        }
    free(key);
    if (d->bar != DUI_BAR_NONE) {
        uint8_t *bar = malloc((size_t)d->bar_w * d->bar_h * 3);
        dui_render_ipe_bar(e, now, d->bar_w, d->bar_h, bar);
        const int y = PAD + gh + GAP;
        if (d->bar == DUI_BAR_STRIP) {
            blit_scaled(&im, bar, d->bar_w, d->bar_h, PAD, y, gw, bar_h);
            for (int i = 0; i < d->dials; i++)
                disc(&im, PAD + gw * (2 * i + 1) / (2.0f * d->dials), (float)(y + bar_h + GAP + 28), 26, 0x2A2F37, false);
        } else {
            const int x = PAD + (gw - d->bar_w) / 2;
            blit(&im, bar, d->bar_w, d->bar_h, x, y, 6);
            disc(&im, (float)(x - 30), y + d->bar_h / 2.0f, 16, SUI_RGB_ACCENT, true);
            disc(&im, (float)(x + d->bar_w + 30), y + d->bar_h / 2.0f, 16, SUI_RGB_ACCENT, true);
        }
        free(bar);
    }
    return im;
}

/* ---- the screens ---- */

typedef struct {
    const char *name;
    void (*build)(dui_screen_t *, const fx_box_t *);
    int scen;
    int ap; /* 1 on, 0 off, 2 on and open */
    int variant; /* V_*, | V_SLOT_OLDER for the older slot as well */
} shot_t;

enum { V_NONE, V_BACKUP, V_ETH, V_TOAST, V_CLOSE, V_LOCKED, V_OLDER, V_SYNCING, V_SLOT_OLDER = 0x100 };

/* Every button state and the icons not on the screens, for looking at. */
static void buttons(dui_screen_t *s, const fx_box_t *b)
{
    dui_screen_init(s);
    fx_status(s, b);
    dui_title(s, "Buttons");
    dui_group(s, DUI_ZONE_MAIN, 0);
    dui_button(s, DUI_ICON_NET, "Network", 1);
    dui_button(s, DUI_ICON_SAT, "Satellite", 1)->flags |= DUI_F_SELECTED;
    dui_button(s, DUI_ICON_RETRY, "Retry now", 1)->flags |= DUI_F_DOWN;
    dui_tile_t *t = dui_button(s, DUI_ICON_RESTART, "Restart", 1);
    t->flags |= DUI_F_DANGER;
    t->hold_ms = 2000;
    t = dui_button(s, DUI_ICON_POWER, "Turn off", 1);
    t->flags |= DUI_F_DANGER | DUI_F_DOWN | DUI_F_ON;
    t->hold_ms = 1500;
    t->hold = 128;
    t = dui_button(s, DUI_ICON_DOCK, "Dock", 1);
    t->hold_ms = 1500;
    t->hold = 200;
    t->flags |= DUI_F_DOWN;
    t = dui_button(s, DUI_ICON_DOCK, "Dock", 1);
    t->hold_ms = 1500;
    t->hint = dui_str(s, "restarts");
    t->flags |= DUI_F_HINT;
    dui_group(s, DUI_ZONE_MAIN, 0);
    t = dui_button(s, DUI_ICON_RESTART, "Restart", 1);
    t->flags |= DUI_F_DANGER | DUI_F_LOCKED;
    t->hold_ms = 2000;
    dui_button(s, DUI_ICON_SYNC, "Sync now", 1)->flags |= DUI_F_LOCKED;
    dui_button(s, DUI_ICON_HOTSPOT, "Hotspot", 1)->state = DUI_STATE_OK;
    dui_button(s, DUI_ICON_COMPANION, "Companion", 1)->state = DUI_STATE_BAD;
    dui_choice(s, "Studio-Mac", "192.168.8.155", 1);
    dui_button(s, DUI_ICON_MUTE, "Sound off", 1);
    dui_button(s, DUI_ICON_LOCK, "Locked", 1);
    dui_group(s, DUI_ZONE_MAIN, 0);
    static const int ICONS[] = {DUI_ICON_WIFI, DUI_ICON_NONET, DUI_ICON_ETH, DUI_ICON_QR, DUI_ICON_LIST, DUI_ICON_CLOSE, DUI_ICON_MENU, DUI_ICON_BACK, DUI_ICON_LOGO, DUI_ICON_CIRCUITRY};
    for (size_t i = 0; i < sizeof ICONS / sizeof ICONS[0]; i++) dui_button(s, ICONS[i], "Icon", 1);
}

static const shot_t SHOTS[] = {
    {"home-nonet", fx_home, FX_NONET, 1, V_NONE},
    {"home-nonet-open", fx_home, FX_NONET, 2, V_NONE},
    {"home-nonet-apoff", fx_home, FX_NONET, 0, V_NONE},
    {"home-finding", fx_home, FX_FINDING, 1, V_NONE},
    {"home-choose", fx_home, FX_CHOOSE, 1, V_NONE},
    {"home-old", fx_home, FX_OLD, 1, V_NONE},
    {"home-dock", fx_home, FX_DOCK, 1, V_NONE},
    {"home-dock-apoff", fx_home, FX_DOCK, 0, V_NONE},
    {"home-recon", fx_home, FX_RECON, 1, V_NONE},
    {"home-connected-ondemand", fx_home, FX_CONNECTED, 1, V_CLOSE},
    {"home-dock-toast", fx_home, FX_DOCK, 1, V_TOAST},
    {"home-nodock", fx_home, FX_NODOCK, 1, V_NONE},
    {"menu", fx_menu, FX_DOCK, 1, V_NONE},
    {"network-main", fx_network, FX_DOCK, 1, V_NONE},
    {"network-backup", fx_network, FX_DOCK, 1, V_BACKUP},
    {"network-eth", fx_network, FX_DOCK, 1, V_ETH},
    {"network-nonet", fx_network, FX_NONET, 1, V_NONE},
    {"companion-dock", fx_companion, FX_DOCK, 1, V_NONE},
    {"companion-sat", fx_companion, FX_FINDING, 1, V_NONE},
    {"companion-locked", fx_companion, FX_FINDING, 1, V_LOCKED},
    {"hotspot", fx_hotspot, FX_DOCK, 1, V_NONE},
    {"hotspot-open", fx_hotspot, FX_DOCK, 2, V_NONE},
    {"hotspot-off", fx_hotspot, FX_DOCK, 0, V_NONE},
    {"qr", fx_qr, FX_DOCK, 1, V_NONE},
    {"box-synced", fx_box, FX_DOCK, 1, V_NONE},
    {"box-older", fx_box, FX_DOCK, 1, V_OLDER},
    {"box-syncing", fx_box, FX_DOCK, 1, V_SYNCING},
    {"box-locked", fx_box, FX_DOCK, 1, V_LOCKED | V_SLOT_OLDER},
    {"buttons", buttons, FX_DOCK, 1, V_NONE},
};
#define NSHOTS ((int)(sizeof SHOTS / sizeof SHOTS[0]))

static fx_box_t box_of(const shot_t *sh)
{
    fx_box_t b = fx_box_default();
    b.scen = sh->scen;
    b.ap = sh->ap != 0;
    b.ap_open = sh->ap == 2;
    const int v = sh->variant & 0xFF;
    b.backup = v == V_BACKUP;
    b.eth = v == V_ETH;
    b.toast = v == V_TOAST ? "Restarting" : NULL;
    b.close = v == V_CLOSE;
    b.locked = v == V_LOCKED;
    b.slot = v == V_OLDER || (sh->variant & V_SLOT_OLDER) ? FX_SLOT_OLDER : v == V_SYNCING ? FX_SLOT_SYNCING : FX_SLOT_SYNCED;
    b.slot_pct = 45;
    return b;
}

/* ---- the real builders (screens.h) on a few snapshots ---- */

/* The default box: dock mode waiting for Companion, hotspot on, main Wi-Fi, the slot in sync. */
static dui_box_t real_box(void)
{
    dui_box_t b;
    memset(&b, 0, sizeof b);
    b.mode = DUI_MODE_DOCK;
    b.sat.state = DUI_SAT_FINDING;
    static const dui_comp_t COMPS[] = {{"Studio-Mac", "192.168.8.155"}, {"FOH-PC", "192.168.8.40"}, {"Edit-Bay", "192.168.8.61"}};
    b.sat.ncomps = 3;
    memcpy(b.sat.comp, COMPS, sizeof COMPS);
    b.sat.target = COMPS[0];
    strcpy(b.sat.version, "4.2.0");
    b.dock.port = 5343;
    strcpy(b.net.wifi_main, "Simbox-Home");
    strcpy(b.net.wifi_backup, "Office-5G");
    b.net.wifi_net = DUI_WIFI_MAIN;
    b.net.rssi = -58;
    strcpy(b.net.wifi_ip, "192.168.8.225");
    b.net.ap_on = true;
    strcpy(b.net.ap_ssid, "CompanionBox-8D1C");
    strcpy(b.net.ap_pass, "k7m2p9xq");
    b.net.ap_secured = true;
    strcpy(b.net.ap_ip, "192.168.4.1");
    strcpy(b.net.name, "CompanionBox-8D1C");
    strcpy(b.box.version, "0.1.0");
    b.box.uptime_s = 2 * 3600 + 14 * 60;
    b.box.slot = DUI_SLOT_IN_SYNC;
    b.box.pct = 45;
    strcpy(b.box.other_version, "0.0.9");
    b.box.other_valid = true;
    b.brightness = 70;
    b.sound = true;
    return b;
}

enum { R_NONE, R_NONET, R_NONET_OPEN, R_CHOOSE, R_ONDEMAND_SAT, R_ONDEMAND_DOCK, R_BACKUP, R_SAT, R_LOCKED,
       R_NET_JOIN, R_NET_ETH, R_NET_OFF, R_NET_NOWIFI, R_TOAST_OK, R_TOAST_WAIT, R_TOAST_BAD, R_FINDING, R_OLD, R_NODOCK,
       R_CONNECTED, R_NONET_NOAP, R_SAT_AUTO, R_SAT_LOCKED };

typedef struct {
    const char *name;
    int page;  /* DUI_PAGE_* */
    int setup; /* R_* */
    int slot;  /* DUI_SLOT_* */
} real_t;

static const real_t REAL[] = {
    {"home-dock", DUI_PAGE_HOME, R_NONE, DUI_SLOT_IN_SYNC},
    {"home-nonet", DUI_PAGE_HOME, R_NONET, DUI_SLOT_IN_SYNC},
    {"home-nonet-open", DUI_PAGE_HOME, R_NONET_OPEN, DUI_SLOT_IN_SYNC},
    {"home-nonet-noap", DUI_PAGE_HOME, R_NONET_NOAP, DUI_SLOT_IN_SYNC}, /* "Plug in cable" */
    {"home-sat-choose", DUI_PAGE_HOME, R_CHOOSE, DUI_SLOT_IN_SYNC},
    {"home-ondemand-sat", DUI_PAGE_HOME, R_ONDEMAND_SAT, DUI_SLOT_IN_SYNC},
    {"home-ondemand-dock", DUI_PAGE_HOME, R_ONDEMAND_DOCK, DUI_SLOT_IN_SYNC},
    {"home-finding", DUI_PAGE_HOME, R_FINDING, DUI_SLOT_IN_SYNC}, /* every status word set */
    {"home-recon", DUI_PAGE_HOME, R_SAT, DUI_SLOT_IN_SYNC},
    {"home-old", DUI_PAGE_HOME, R_OLD, DUI_SLOT_IN_SYNC},
    {"home-nodock", DUI_PAGE_HOME, R_NODOCK, DUI_SLOT_IN_SYNC},
    {"home-connected", DUI_PAGE_HOME, R_CONNECTED, DUI_SLOT_IN_SYNC},
    {"menu", DUI_PAGE_MENU, R_NONE, DUI_SLOT_DIFFERS},
    {"network", DUI_PAGE_NETWORK, R_NONE, DUI_SLOT_IN_SYNC},
    {"network-backup", DUI_PAGE_NETWORK, R_BACKUP, DUI_SLOT_IN_SYNC},
    {"network-joining", DUI_PAGE_NETWORK, R_NET_JOIN, DUI_SLOT_IN_SYNC},
    {"network-eth", DUI_PAGE_NETWORK, R_NET_ETH, DUI_SLOT_IN_SYNC},
    {"network-wifi-off", DUI_PAGE_NETWORK, R_NET_OFF, DUI_SLOT_IN_SYNC},
    {"network-no-wifi", DUI_PAGE_NETWORK, R_NET_NOWIFI, DUI_SLOT_IN_SYNC},
    {"locked-network", DUI_PAGE_NETWORK, R_LOCKED, DUI_SLOT_IN_SYNC},
    {"toast-done-home", DUI_PAGE_HOME, R_TOAST_OK, DUI_SLOT_IN_SYNC},
    {"toast-busy-network", DUI_PAGE_NETWORK, R_TOAST_WAIT, DUI_SLOT_IN_SYNC},
    {"toast-refused-box", DUI_PAGE_BOX, R_TOAST_BAD, DUI_SLOT_IN_SYNC},
    {"toast-refused-menu", DUI_PAGE_MENU, R_TOAST_BAD, DUI_SLOT_IN_SYNC},
    {"companion-dock", DUI_PAGE_COMPANION, R_NONE, DUI_SLOT_IN_SYNC},
    {"companion-sat", DUI_PAGE_COMPANION, R_SAT, DUI_SLOT_IN_SYNC},           /* a manual Companion: Auto find, Set IP */
    {"companion-sat-auto", DUI_PAGE_COMPANION, R_SAT_AUTO, DUI_SLOT_IN_SYNC}, /* auto-discovery: Choose, Set IP */
    {"companion-sat-connected", DUI_PAGE_COMPANION, R_CONNECTED, DUI_SLOT_IN_SYNC},
    {"hotspot", DUI_PAGE_HOTSPOT, R_NONE, DUI_SLOT_IN_SYNC},
    {"qr", DUI_PAGE_QR, R_NONE, DUI_SLOT_IN_SYNC},
    {"box-in_sync", DUI_PAGE_BOX, R_NONE, DUI_SLOT_IN_SYNC},
    {"box-syncing", DUI_PAGE_BOX, R_NONE, DUI_SLOT_SYNCING},
    {"box-differs", DUI_PAGE_BOX, R_NONE, DUI_SLOT_DIFFERS},
    {"box-other_newer", DUI_PAGE_BOX, R_NONE, DUI_SLOT_OTHER_NEWER},
    {"box-empty", DUI_PAGE_BOX, R_NONE, DUI_SLOT_EMPTY},
    {"box-invalid", DUI_PAGE_BOX, R_NONE, DUI_SLOT_INVALID},
    {"box-waiting", DUI_PAGE_BOX, R_NONE, DUI_SLOT_WAITING},
    {"locked-companion", DUI_PAGE_COMPANION, R_LOCKED, DUI_SLOT_IN_SYNC},
    {"locked-companion-sat", DUI_PAGE_COMPANION, R_SAT_LOCKED, DUI_SLOT_IN_SYNC}, /* Auto find and Set IP locked */
    {"locked-hotspot", DUI_PAGE_HOTSPOT, R_LOCKED, DUI_SLOT_IN_SYNC},
    {"locked-box-differs", DUI_PAGE_BOX, R_LOCKED, DUI_SLOT_DIFFERS},
    {"locked-box-other_newer", DUI_PAGE_BOX, R_LOCKED, DUI_SLOT_OTHER_NEWER},
};
#define NREAL ((int)(sizeof REAL / sizeof REAL[0]))

static void real_build(dui_screen_t *s, const real_t *r)
{
    dui_box_t b = real_box();
    b.box.slot = (uint8_t)r->slot;
    switch (r->setup) {
    case R_NONET_NOAP:
        b.net.ap_on = false;
        /* fall through */
    case R_NONET_OPEN:
        b.net.ap_secured = false;
        /* fall through */
    case R_NONET:
        b.net.wifi_net = DUI_WIFI_NONE;
        b.net.wifi_ip[0] = '\0';
        break;
    case R_CHOOSE:
        b.mode = DUI_MODE_SAT;
        b.sat.state = DUI_SAT_CHOOSE;
        break;
    case R_ONDEMAND_SAT:
        b.mode = DUI_MODE_SAT;
        b.sat.state = DUI_SAT_CONNECTED;
        b.connected = b.ondemand = true;
        break;
    case R_ONDEMAND_DOCK:
        strcpy(b.dock.peer, "192.168.8.155");
        b.connected = b.ondemand = true;
        break;
    case R_BACKUP:
        b.net.wifi_net = DUI_WIFI_BACKUP;
        b.net.rssi = -66;
        break;
    case R_SAT_LOCKED:
        b.locked = true;
        /* fall through */
    case R_SAT:
        b.mode = DUI_MODE_SAT;
        b.sat.state = DUI_SAT_RECON;
        break;
    case R_SAT_AUTO:
        b.mode = DUI_MODE_SAT;
        b.sat.state = DUI_SAT_RECON;
        b.sat.auto_disc = true;
        break;
    case R_FINDING: b.mode = DUI_MODE_SAT; break;
    case R_OLD:
        b.mode = DUI_MODE_SAT;
        b.sat.state = DUI_SAT_TOO_OLD;
        break;
    case R_NODOCK: b.dock.no_free = true; break;
    case R_CONNECTED:
        b.mode = DUI_MODE_SAT;
        b.sat.state = DUI_SAT_CONNECTED;
        b.connected = true;
        break;
    case R_LOCKED: b.locked = true; break;
    case R_NET_JOIN: /* neither joined: Main being tried, Backup not found */
        b.net.wifi_net = DUI_WIFI_NONE;
        b.net.wifi_ip[0] = '\0';
        b.net.wifi_trying = DUI_WIFI_MAIN;
        b.net.wifi_not_found = 1u << (DUI_WIFI_BACKUP - 1);
        break;
    case R_NET_ETH: /* Ethernet up on a static address (it routes), Main joined behind it: both addresses */
        b.net.eth_link = true;
        b.net.eth_dhcp = false;
        strcpy(b.net.eth_ip, "192.168.100.200");
        break;
    case R_NET_OFF: b.net.wifi_off = true; break;
    case R_TOAST_OK:
        strcpy(b.toast, "Static 192.168.8.50");
        b.toast_state = DUI_STATE_OK;
        break;
    case R_TOAST_WAIT:
        strcpy(b.toast, "Switching");
        b.toast_state = DUI_STATE_WAIT;
        break;
    case R_TOAST_BAD:
        strcpy(b.toast, "Upload running");
        b.toast_state = DUI_STATE_BAD;
        break;
    case R_NET_NOWIFI:
        b.net.wifi_main[0] = b.net.wifi_backup[0] = '\0';
        b.net.wifi_net = DUI_WIFI_NONE;
        b.net.wifi_ip[0] = '\0';
        break;
    default: break;
    }
    dui_build(s, r->page, &b);
}

/* ---- the contact sheet ---- */

static void label(image_t *im, int x, int y, const char *s, uint32_t rgb)
{
    const dui_canvas_t c = {im->rgb, im->w, im->h};
    dui__text(&c, 20, (float)x, (float)y, s, (int)strlen(s), rgb, 0, DUI__LEFT);
}

typedef struct {
    image_t im;
    char name[64];
} page_t;

#define PAGES_MAX 160

static int pin_renders(const char *dir);
static int ipe_renders(const char *dir);
static int hint_renders(const char *dir);
static int icon_renders(const char *dir);

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[2], "pin")) return pin_renders(argv[1]);
    if (argc == 3 && !strcmp(argv[2], "ipedit")) return ipe_renders(argv[1]);
    if (argc == 3 && !strcmp(argv[2], "hints")) return hint_renders(argv[1]);
    if (argc == 3 && !strcmp(argv[2], "icons")) return icon_renders(argv[1]);
    if (argc != 2 && !(argc == 3 && !strcmp(argv[2], "screens"))) {
        fprintf(stderr, "usage: %s <out dir> [screens|pin|ipedit|hints|icons]\n", argv[0]);
        return 2;
    }
    const bool real = argc == 3;
    const int nshots = real ? NREAL : NSHOTS;
    static const struct { const char *name; uint16_t pid; int sheet_cols; } M[] = {
        {"module32-8x4-96", 0x00BA, 3}, {"plusxl-9x4-112", 0x00C6, 2}, {"mk2-5x3-72", 0x0080, 5},
        {"plus-4x2-120", 0x0084, 4},     {"neo-4x2-96", 0x009A, 5},     {"mini-3x2-80", 0x0063, 6},
    };
    static dui_screen_t s;
    static dui_layout_t L;
    static page_t pages[PAGES_MAX];
    int fails = 0;
    for (size_t m = 0; m < sizeof M / sizeof M[0]; m++) {
        dui_deck_t d;
        if (!dui_deck_of(deck_model_find(DECK_VID_ELGATO, M[m].pid), &d)) return 1;
        int np = 0;
        for (int k = 0; k < nshots; k++) {
            const char *name = real ? REAL[k].name : SHOTS[k].name;
            if (real) {
                real_build(&s, &REAL[k]);
            } else {
                const fx_box_t b = box_of(&SHOTS[k]);
                SHOTS[k].build(&s, &b);
            }
            dui_plan(&d, &s, &L);
            if (s.overflow || L.overflow) {
                fprintf(stderr, "%s %s: overflow\n", M[m].name, name);
                fails++;
            }
            for (int p = 0; p < L.pages && np < PAGES_MAX; p++) {
                image_t im = deck_page(&d, &s, &L, p);
                char path[512];
                snprintf(path, sizeof path, "%s/%s_%s_p%d.png", argv[1], M[m].name, name, p + 1);
                if (write_png(path, im.w, im.h, im.rgb) != 0) fails++;
                pages[np].im = im;
                snprintf(pages[np].name, sizeof pages[np].name, "%s  %d/%d", name, p + 1, L.pages);
                np++;
            }
        }
        /* the contact sheet: every page of every screen */
        const int cols = M[m].sheet_cols, cw = pages[0].im.w + 16;
        int maxh = 0;
        for (int i = 0; i < np; i++)
            if (pages[i].im.h > maxh) maxh = pages[i].im.h;
        const int rows = (np + cols - 1) / cols;
        image_t sheet = image(cols * cw + 16, rows * (maxh + 40) + 16, SHEET_BG);
        for (int i = 0; i < np; i++) {
            const int x = 16 + (i % cols) * cw, y = 16 + (i / cols) * (maxh + 40);
            label(&sheet, x, y + 20, pages[i].name, 0xC8CCD2);
            blit(&sheet, pages[i].im.rgb, pages[i].im.w, pages[i].im.h, x, y + 30, 0);
            free(pages[i].im.rgb);
        }
        char path[512];
        snprintf(path, sizeof path, "%s/%s_sheet.png", argv[1], M[m].name);
        if (write_png(path, sheet.w, sheet.h, sheet.rgb) != 0) fails++;
        printf("%s: %d pages, %s\n", M[m].name, np, path);
        free(sheet.rgb);
    }
    if (real) return fails ? 1 : 0;
    /* the QR module size of the hotspot's join string and the setup URL per key size (one key, a
     * 1-module quiet zone); the ECC level: M when it fits the version dui_qr_encode chose */
    char join[128];
    dui_wifi_join(join, sizeof join, "CompanionBox-8D1C", "k7m2xq9fhd"); /* a 10-char hotspot password */
    const char *PAY[] = {join, "http://192.168.4.1/"};
    for (int k = 0; k < 2; k++) {
        uint8_t qr[DUI_QR_BUF], tmp[DUI_QR_BUF];
        if (!dui_qr_encode(PAY[k], qr)) continue;
        const int n = dui_qr_size(qr), v = (n - 17) / 4;
        const bool m = qrcodegen_encodeText(PAY[k], tmp, qr, qrcodegen_Ecc_MEDIUM, v, v, qrcodegen_Mask_AUTO, false);
        printf("QR \"%s\" (%d bytes): version %d, ECC %s, %d modules + 2 quiet", PAY[k], (int)strlen(PAY[k]), v,
               m ? "M" : "L", n);
        static const int KEYS[] = {72, 80, 96, 112, 120};
        for (size_t i = 0; i < 5; i++) printf(", %d px key: %d px", KEYS[i], dui_qr_module_px(KEYS[i], n));
        printf("\n");
    }
    return fails ? 1 : 0;
}

/* The PIN pad on every model: empty, 3 digits in (a key held), Checking, Wrong PIN, Locked, no PIN set;
 * the + and Neo's second page of digits; a contact sheet per model. */
static int pin_renders(const char *dir)
{
    static const struct { const char *name; uint16_t pid; } M[] = {
        {"module32-8x4-96", 0x00BA}, {"xl-8x4-96", 0x006C},  {"plusxl-9x4-112", 0x00C6}, {"mk2-5x3-72", 0x0080},
        {"plus-4x2-120", 0x0084},     {"neo-4x2-96", 0x009A}, {"mini-3x2-80", 0x0063},
    };
    enum { S_EMPTY, S_TYPED, S_PAGE2, S_CHECKING, S_WRONG, S_LOCKED, S_NOPIN, S_USE_MAIN, S_USE_BACKUP, S_N };
    static const char *SN[S_N] = {"empty", "3-digits", "page2", "checking", "wrong", "locked", "no-pin", "use-primary",
                                  "use-backup"}; /* the Use pads name their network */
    static dui_layout_t L;
    static page_t pages[PAGES_MAX];
    int fails = 0;
    for (size_t m = 0; m < sizeof M / sizeof M[0]; m++) {
        dui_deck_t d;
        if (!dui_deck_of(deck_model_find(DECK_VID_ELGATO, M[m].pid), &d)) return 1;
        int np = 0;
        for (int st = 0; st < S_N; st++) {
            dui_pin_t p;
            dui_pin_out_t o;
            const int64_t now = 100000;
            const uint16_t act = st == S_USE_MAIN ? DUI_A_USE_MAIN : st == S_USE_BACKUP ? DUI_A_USE_BACKUP : DUI_A_RESTART;
            dui_pin_open(&p, &d, act, st != S_NOPIN, st == S_LOCKED ? 37 : 0, now, &o);
            if (st == S_PAGE2 && p.style != DUI_PIN_PAGES) continue;
            if (st == S_TYPED || st == S_PAGE2 || st == S_CHECKING || st == S_WRONG) {
                memcpy(p.d, "471", 4);
                p.n = 3;
                p.roller = 1;
            }
            if (st == S_PAGE2) p.pg = 1;
            if (st == S_CHECKING || st == S_WRONG) {
                p.phase = DUI_PIN_CHECKING;
                memset(p.d, 0, sizeof p.d);
                p.n = 0;
            }
            if (st == S_WRONG) dui_pin_result(&p, DUI_PIN_R_WRONG, 0, now, &o);
            dui_pin_layout(&d, &p, now, &L);
            if (st == S_TYPED) { /* a key held: the first digit key */
                for (int r = 0; r < d.rows && p.held.col < 0; r++)
                    for (int c = 0; c < d.cols && p.held.col < 0; c++) {
                        dui_tile_t t;
                        if (dui_layout_key(&L, p.pg, c, r, &t) && t.action == DUI_PIN_A_DIGIT + 5) {
                            p.held.col = (int8_t)c;
                            p.held.row = (int8_t)r;
                        }
                    }
            }
            if (L.overflow) {
                fprintf(stderr, "%s pin %s: overflow\n", M[m].name, SN[st]);
                fails++;
            }
            image_t im = pin_page(&d, &p, &L, p.pg, now);
            char path[512];
            snprintf(path, sizeof path, "%s/pin_%s_%s.png", dir, M[m].name, SN[st]);
            if (write_png(path, im.w, im.h, im.rgb) != 0) fails++;
            if (np < PAGES_MAX) {
                pages[np].im = im;
                snprintf(pages[np].name, sizeof pages[np].name, "%s", SN[st]);
                np++;
            } else {
                free(im.rgb);
            }
        }
        const int cols = 4, cw = pages[0].im.w + 16;
        int maxh = 0;
        for (int i = 0; i < np; i++)
            if (pages[i].im.h > maxh) maxh = pages[i].im.h;
        const int rows = (np + cols - 1) / cols;
        image_t sheet = image(cols * cw + 16, rows * (maxh + 40) + 16, SHEET_BG);
        for (int i = 0; i < np; i++) {
            const int x = 16 + (i % cols) * cw, y = 16 + (i / cols) * (maxh + 40);
            label(&sheet, x, y + 20, pages[i].name, 0xC8CCD2);
            blit(&sheet, pages[i].im.rgb, pages[i].im.w, pages[i].im.h, x, y + 30, 0);
            free(pages[i].im.rgb);
        }
        char path[512];
        snprintf(path, sizeof path, "%s/pin_%s_sheet.png", dir, M[m].name);
        if (write_png(path, sheet.w, sheet.h, sheet.rgb) != 0) fails++;
        printf("%s: %d PIN pad states, %s\n", M[m].name, np, path);
        free(sheet.rgb);
    }
    return fails ? 1 : 0;
}

/* The static-address editor on every model: the IP part typed, a page of digits, the Mask, an error,
 * the Gateway, the DNS empty, the summary with Save held half way; then its host mode: the Companion IP
 * prefilled, typed, an error, the summary with Save held half way. */
static int ipe_renders(const char *dir)
{
    static const struct { const char *name; uint16_t pid; } M[] = {
        {"module32-8x4-96", 0x00BA}, {"xl-8x4-96", 0x006C},  {"plusxl-9x4-112", 0x00C6}, {"mk2-5x3-72", 0x0080},
        {"plus-4x2-120", 0x0084},     {"neo-4x2-96", 0x009A}, {"mini-3x2-80", 0x0063},
        {"orig-5x3-72", 0x0060},      {"module15-5x3-72", 0x00B9}, {"module6-3x2-80", 0x00B8},
    };
    enum { S_IP, S_PAGE2, S_MASK, S_ERROR, S_GW, S_DNS, S_SUM, S_SUM2, S_SUM3,
           S_HIP, S_HTYPE, S_HPAGE2, S_HERR, S_HSUM, S_HSUM2, S_HSUM3, S_N };
    static const char *SN[S_N] = {"ip-typing",   "page2",       "mask",          "error",       "gateway",    "dns-empty",
                                  "summary-p1",  "summary-p2",  "summary-p3",    "host-ip",     "host-typing", "host-page2",
                                  "host-error",  "host-summary-p1", "host-summary-p2", "host-summary-p3"};
    static dui_layout_t L;
    static dui_ipe_t e;
    static page_t pages[PAGES_MAX];
    int fails = 0;
    const dui_ipset_t pre = {"192.168.8.50", "255.255.255.0", "192.168.8.1", ""};
    for (size_t m = 0; m < sizeof M / sizeof M[0]; m++) {
        dui_deck_t d;
        if (!dui_deck_of(deck_model_find(DECK_VID_ELGATO, M[m].pid), &d)) return 1;
        int np = 0;
        for (int st = 0; st < S_N; st++) {
            dui_ipe_out_t o;
            const int64_t now = 100000;
            const bool host = st >= S_HIP, sumst = (st >= S_SUM && st <= S_SUM3) || st >= S_HSUM;
            const int sum0 = host ? S_HSUM : S_SUM;
            if (host) dui_ipe_open_host(&e, &d, "192.168.8.155", 16622, now, &o);
            else dui_ipe_open(&e, &d, &pre, now, &o);
            if ((st == S_PAGE2 || st == S_HPAGE2) && e.style != DUI_PIN_PAGES) continue;
            if (st == S_IP || st == S_PAGE2 || st == S_HTYPE || st == S_HPAGE2) { /* 10.20.3 typed, on the third octet */
                memset(&e.q[0], 0, sizeof e.q[0]);
                for (const char *k = "10"; *k; k++) dui_ipe_digit(&e, *k - '0');
                dui_ipe_next(&e);
                for (const char *k = "203"; *k; k++) dui_ipe_digit(&e, *k - '0');
                e.q[0].set &= 0x07;
                if (st == S_PAGE2 || st == S_HPAGE2) e.pg = 1;
            }
            if (st == S_HERR) { /* 127.0.0.1: not a host's */
                e.q[0] = (dui_quad_t){{127, 0, 0, 1}, 0x0F};
                e.oct = 3;
                e.fresh = false;
                e.err = DUI_IPE_E_RESERVED;
                e.err_until = now + 1000;
            }
            if (st == S_MASK) e.field = DUI_IPE_MASK;
            if (st == S_ERROR || st == S_GW) {
                e.field = DUI_IPE_GW;
                e.oct = 1;
                e.fresh = false;
            }
            if (st == S_ERROR) {
                e.q[1] = (dui_quad_t){{10, 0, 0, 1}, 0x0F};
                e.err = DUI_IPE_E_GW_SUBNET;
                e.err_until = now + 1000;
            }
            if (st == S_DNS) e.field = DUI_IPE_DNS;
            if (sumst) e.field = DUI_IPE_SUM;
            dui_ipe_layout(&d, &e, now, &L);
            if (sumst) { /* every page of the summary (the values, then Save on the paged decks) */
                if (st - sum0 >= L.pages) continue;
                e.pg = (uint8_t)(st - sum0);
                for (int r = 0; r < d.rows; r++) /* Save held half way where it shows */
                    for (int c = 0; c < d.cols; c++) {
                        dui_tile_t t;
                        if (dui_layout_key(&L, e.pg, c, r, &t) && t.action == DUI_IPE_A_SAVE) {
                            e.held.col = (int8_t)c;
                            e.held.row = (int8_t)r;
                            e.held.action = DUI_IPE_A_SAVE;
                            e.held.at = now - DUI_HOLD_ETH_MS / 2;
                        }
                    }
            }
            if (L.overflow) {
                fprintf(stderr, "%s ipedit %s: overflow\n", M[m].name, SN[st]);
                fails++;
            }
            image_t im = ipe_page(&d, &e, &L, e.pg, now);
            char path[512];
            snprintf(path, sizeof path, "%s/ipedit_%s_%s.png", dir, M[m].name, SN[st]);
            if (write_png(path, im.w, im.h, im.rgb) != 0) fails++;
            if (np < PAGES_MAX) {
                pages[np].im = im;
                snprintf(pages[np].name, sizeof pages[np].name, "%s", SN[st]);
                np++;
            } else {
                free(im.rgb);
            }
        }
        const int cols = 4, cw = pages[0].im.w + 16;
        int maxh = 0;
        for (int i = 0; i < np; i++)
            if (pages[i].im.h > maxh) maxh = pages[i].im.h;
        const int rows = (np + cols - 1) / cols;
        image_t sheet = image(cols * cw + 16, rows * (maxh + 40) + 16, SHEET_BG);
        for (int i = 0; i < np; i++) {
            const int x = 16 + (i % cols) * cw, y = 16 + (i / cols) * (maxh + 40);
            label(&sheet, x, y + 20, pages[i].name, 0xC8CCD2);
            blit(&sheet, pages[i].im.rgb, pages[i].im.w, pages[i].im.h, x, y + 30, 0);
            free(pages[i].im.rgb);
        }
        char path[512];
        snprintf(path, sizeof path, "%s/ipedit_%s_sheet.png", dir, M[m].name);
        if (write_png(path, sheet.w, sheet.h, sheet.rgb) != 0) fails++;
        printf("%s: %d editor states, %s\n", M[m].name, np, path);
        free(sheet.rgb);
    }
    return fails ? 1 : 0;
}

/* every hold button let go early (its instruction in amber), on every key size: one sheet, a row per
 * key size, and the same buttons as they rest. */
static int hint_renders(const char *dir)
{
    static const struct {
        int icon;
        const char *label, *hint;
        uint16_t hold_ms;
        uint16_t flags;
    } B[] = {
        {DUI_ICON_RESTART, "Restart", "restarts", DUI_HOLD_RESTART_MS, DUI_F_DANGER},
        {DUI_ICON_RESTART, "Boot other", "restarts", DUI_HOLD_BOOT_MS, DUI_F_DANGER},
        {DUI_ICON_DOCK, "Dock", "restarts", DUI_HOLD_MODE_MS, 0},
        {DUI_ICON_POWER, "Turn off", "cuts users", DUI_HOLD_AP_OFF_MS, DUI_F_DANGER | DUI_F_ON},
        {DUI_ICON_WIFI, "Use", "Wi-Fi blips", DUI_HOLD_USE_MS, 0},
        {DUI_ICON_POWER, "Wi-Fi", "Wi-Fi off", DUI_HOLD_WIFI_MS, DUI_F_ON | DUI_F_SELECTED},
        {DUI_ICON_ETH, "Use DHCP", "net resets", DUI_HOLD_ETH_MS, 0},
        {DUI_ICON_ETH, "Save", "net resets", DUI_HOLD_ETH_MS, 0},
    };
    static const int KEYS[] = {72, 80, 96, 112, 120};
    const int nb = (int)(sizeof B / sizeof B[0]), cell = 120 + 12;
    image_t sheet = image(16 + nb * cell, 16 + 10 * (cell + 20), SHEET_BG);
    for (int k = 0; k < 5; k++)
        for (int pass = 0; pass < 2; pass++) {
            const int px = KEYS[k], y = 16 + (k * 2 + pass) * (cell + 20);
            char name[48];
            snprintf(name, sizeof name, "%d px, %s", px, pass ? "at rest" : "let go early");
            label(&sheet, 16, y + 16, name, 0xC8CCD2);
            for (int i = 0; i < nb; i++) {
                dui_tile_t t;
                memset(&t, 0, sizeof t);
                t.kind = DUI_TILE_BUTTON;
                t.tint = DUI_TINT_BLACK;
                t.icon = (uint8_t)B[i].icon;
                t.text = B[i].label;
                t.hint = B[i].hint;
                t.hold_ms = B[i].hold_ms;
                t.flags = (uint16_t)(B[i].flags | (pass ? 0 : DUI_F_HINT));
                uint8_t *rgb = malloc((size_t)px * px * 3);
                if (!rgb) return 1;
                dui_render_key(&t, px, rgb);
                blit(&sheet, rgb, px, px, 16 + i * cell, y + 24, 0);
                free(rgb);
            }
        }
    char path[512];
    snprintf(path, sizeof path, "%s/hints.png", dir);
    const int fails = write_png(path, sheet.w, sheet.h, sheet.rgb) != 0;
    printf("hints: %s\n", path);
    free(sheet.rgb);
    return fails;
}

/* every icon on one sheet: a row of buttons on 72, 96 and 120 px keys (the Wi-Fi bars at 3, 2 and 1, the toggle
 * off and on), then each icon alone at 72, 96 and 120 px, white on the black of a key. */
static int icon_renders(const char *dir)
{
    static const char *const NAMES[] = {"", "Wi-Fi", "No net", "Ethernet", "Network", "Deck", "Dock", "Hotspot", "Info",
                                        "Back", "Fwd", "Menu", "Retry", "Restart", "Sync", "Toggle", "QR", "List",
                                        "Dimmer", "Brighter", "Sound", "Mute", "Lock", "Close", "Logo", "Delete", "Sat",
                                        "Flash", "Edit", "Companion", "Circuitry", "Search", "Wi-Fi off", "Hotspot off"};
    typedef struct {
        int icon;
        int8_t rssi;
        uint16_t flags;
        const char *label;
    } item_t;
    item_t items[64];
    int n = 0;
    for (int i = 1; i < DUI_ICONS && n < 60; i++) {
        const char *nm = i < (int)(sizeof NAMES / sizeof NAMES[0]) ? NAMES[i] : "?";
        items[n++] = (item_t){i, -50, 0, nm};
        if (i == DUI_ICON_WIFI) {
            items[n++] = (item_t){i, -65, 0, "Wi-Fi 2"};
            items[n++] = (item_t){i, -80, 0, "Wi-Fi 1"};
        }
        if (i == DUI_ICON_POWER) items[n++] = (item_t){i, 0, DUI_F_ON, "Toggle on"};
    }
    static const int KEYS[] = {72, 96, 120};
    const int per = 12, gap = 12;
    int H = 16, W = 16 + per * (120 + gap);
    for (int k = 0; k < 3; k++) H += ((n + per - 1) / per) * (KEYS[k] + gap) + 34;
    for (int k = 0; k < 3; k++) H += ((n + per - 1) / per) * (KEYS[k] + 30) + 34;
    image_t sheet = image(W, H, SHEET_BG);
    int y = 16;
    uint8_t *key = malloc(120 * 120 * 3);
    for (int k = 0; k < 3; k++) { /* as buttons */
        const int px = KEYS[k];
        char t[48];
        snprintf(t, sizeof t, "buttons, %d px keys", px);
        label(&sheet, 16, y + 20, t, 0xC8CCD2);
        y += 30;
        for (int i = 0; i < n; i++) {
            dui_tile_t tile;
            memset(&tile, 0, sizeof tile);
            tile.kind = DUI_TILE_BUTTON;
            tile.icon = (uint8_t)items[i].icon;
            tile.rssi = items[i].rssi;
            tile.flags = items[i].flags;
            tile.text = items[i].label;
            memset(key, 0, (size_t)px * px * 3);
            dui_render_key(&tile, px, key);
            blit(&sheet, key, px, px, 16 + (i % per) * (120 + gap), y + (i / per) * (px + gap), px / 8);
        }
        y += ((n + per - 1) / per) * (px + gap) + 4;
    }
    for (int k = 0; k < 3; k++) { /* alone */
        const int px = KEYS[k];
        char t[48];
        snprintf(t, sizeof t, "alone, %d px", px);
        label(&sheet, 16, y + 20, t, 0xC8CCD2);
        y += 30;
        for (int i = 0; i < n; i++) {
            memset(key, 0, 120 * 120 * 3);
            const dui_canvas_t c = {key, px, px};
            dui_icon(&c, items[i].icon, px / 2.0f, px / 2.0f, (float)px, 0xFFFFFF, items[i].rssi, items[i].flags);
            const int x0 = 16 + (i % per) * (120 + gap), y0 = y + (i / per) * (px + 30);
            blit(&sheet, key, px, px, x0, y0, 0);
            label(&sheet, x0, y0 + px + 20, items[i].label, 0x8A9099);
        }
        y += ((n + per - 1) / per) * (px + 30) + 4;
    }
    free(key);
    char path[512];
    snprintf(path, sizeof path, "%s/icons_sheet.png", dir);
    const int r = write_png(path, sheet.w, sheet.h, sheet.rgb);
    printf("%d icons, %s\n", n, path);
    free(sheet.rgb);
    return r ? 1 : 0;
}
