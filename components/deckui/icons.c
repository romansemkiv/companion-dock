/* Pure: the icons (deckui.h). every generic icon is Phosphor's (phosphoricons.com, MIT,
 * @phosphor-icons/core, Regular weight), its outline as generated contours (phosphor.c, tools/phosphor_icons.py) filled
 * by rs_fill_contours, the 256-unit box on the 24-unit one (Phosphor's proportions and its stroke, PH_STROKE units: 1.5
 * of ours in Regular; all ink inside the box). Companion is Phosphor's webhooks-logo too; Satellite is Lucide's
 * satellite (lucide.dev, ISC; Phosphor has none), drawn as strokes in Phosphor's weight, as is the box's nine-dot logo. */
#include <math.h>

#include "phosphor.h"
#include "raster.h"

#define PI_F 3.14159265f

typedef struct {
    const dui_canvas_t *c;
    float cx, cy, k; /* the box's centre, px per unit */
} ictx_t;

static float X(const ictx_t *I, float u) { return I->cx + u * I->k; }
static float Y(const ictx_t *I, float v) { return I->cy + v * I->k; }

static void seg(rs_shape_t *s, const ictx_t *I, float u0, float v0, float u1, float v1)
{
    rs_seg(s, X(I, u0), Y(I, v0), X(I, u1), Y(I, v1));
}

static void arc(rs_shape_t *s, const ictx_t *I, float u, float v, float r, float a0, float a1)
{
    rs_arc(s, X(I, u), Y(I, v), r * I->k, a0, a1);
}

static void disc(rs_shape_t *s, const ictx_t *I, float u, float v, float r) { rs_disc(s, X(I, u), Y(I, v), r * I->k); }

/* ---- Phosphor's glyphs ---- */

/* px per Phosphor unit: its 256-unit box on our 24-unit one */
static float ph_k(const ictx_t *I) { return I->k * 24.0f / PH_BOX; }

/* Glyph g in rgb at alpha, centred as Phosphor centres it in its box (cut: a capsule cleared from it, or NULL). */
static void glyph(const ictx_t *I, int g, uint32_t rgb, float alpha, const rs_capsule_t *cut)
{
    const float k = ph_k(I);
    rs_fill_contours(I->c, ph_pts, ph_end, ph_glyph[g].c0, ph_glyph[g].nc, I->cx - PH_BOX / 2 * k, I->cy - PH_BOX / 2 * k,
                     k / PH_UNIT, cut, rgb, alpha);
}

/* A point of Phosphor's box in px. */
static float PX(const ictx_t *I, float x) { return I->cx + (x - PH_BOX / 2) * ph_k(I); }
static float PY(const ictx_t *I, float y) { return I->cy + (y - PH_BOX / 2) * ph_k(I); }

/* The Wi-Fi fan (wifi-high, -medium, -low): the bars rssi lights at full ink, the rest of wifi-high at 30 %. */
static void wifi(const ictx_t *I, uint32_t rgb, int8_t rssi)
{
    const int bars = rssi == 0 ? 3 : rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
    if (bars < 3) glyph(I, PH_WIFI_HIGH, rgb, 0.3f, NULL);
    glyph(I, bars == 3 ? PH_WIFI_HIGH : bars == 2 ? PH_WIFI_MEDIUM : PH_WIFI_LOW, rgb, 1, NULL);
}

/* Phosphor's slash (as in its wifi-slash): from (48, 40) to (208, 216) of its box, the weight's stroke (PH_STROKE,
 * round caps). */
static void slash(const ictx_t *I, uint32_t rgb)
{
    rs_shape_t s;
    rs_init(&s, PH_STROKE * ph_k(I));
    rs_seg(&s, PX(I, 48), PY(I, 40), PX(I, 208), PY(I, 216));
    rs_draw(I->c, &s, rgb, 1);
}

/* Satellite mode, Lucide's satellite (ISC, lucide-react 0.525.0) on its own 24-unit box, which is ours (u, v from
 * its centre): the body, a square turned 45 degrees; the two solar panels either side of it, each three sides of a
 * square open toward the body; the antenna to the upper right; the signal arc round the lower-left corner. Its corners
 * (Lucide rounds them by 1.2 units) are the stroke's round joins; Phosphor's stroke (PH_STROKE, 1.5 units) rather than
 * Lucide's 2, so it sits with the deck's other icons. */
static void satellite(rs_shape_t *s, const ictx_t *I, uint32_t rgb)
{
    static const float body[] = {8.5f, 11.5f, 12.5f, 15.5f, 18.5f, 9.5f, 14.5f, 5.5f};
    static const float panel_a[] = {13.5f, 6.5f, 9.5f, 2.5f, 5.5f, 6.5f, 9.5f, 10.5f};
    static const float panel_b[] = {17.5f, 10.5f, 21.5f, 14.5f, 17.5f, 18.5f, 13.5f, 14.5f};
    rs_init(s, PH_STROKE * ph_k(I));
    for (int i = 0; i < 4; i++) { /* closed */
        const int j = (i + 1) % 4;
        seg(s, I, body[2 * i] - 12, body[2 * i + 1] - 12, body[2 * j] - 12, body[2 * j + 1] - 12);
    }
    for (int i = 0; i < 3; i++) { /* open */
        seg(s, I, panel_a[2 * i] - 12, panel_a[2 * i + 1] - 12, panel_a[2 * i + 2] - 12, panel_a[2 * i + 3] - 12);
        seg(s, I, panel_b[2 * i] - 12, panel_b[2 * i + 1] - 12, panel_b[2 * i + 2] - 12, panel_b[2 * i + 3] - 12);
    }
    seg(s, I, 16.5f - 12, 7.5f - 12, 19 - 12, 5 - 12);  /* the antenna */
    arc(s, I, 3 - 12, 21 - 12, 6, -PI_F / 2, 0);         /* the signal: (3, 15) round to (9, 21) */
    rs_draw(I->c, s, rgb, 1);
}

/* the generic icons and their Phosphor glyphs (-1: drawn here otherwise). */
static int ph_of(int icon, uint16_t flags)
{
    switch (icon) {
    case DUI_ICON_ETH: return PH_NETWORK;
    case DUI_ICON_NET: return PH_GLOBE;
    case DUI_ICON_DECK: return PH_SQUARES_FOUR;
    case DUI_ICON_DOCK: return PH_HARD_DRIVE;
    case DUI_ICON_HOTSPOT: return PH_BROADCAST;
    case DUI_ICON_INFO: return PH_INFO;
    case DUI_ICON_BACK: return PH_CARET_LEFT;
    case DUI_ICON_FWD: return PH_CARET_RIGHT;
    case DUI_ICON_MENU: return PH_LIST;
    case DUI_ICON_RETRY: return PH_ARROW_CLOCKWISE;
    case DUI_ICON_RESTART: return PH_POWER; /* restarting the box: the power sign, never a sync-type arrow */
    case DUI_ICON_SYNC: return PH_ARROWS_CLOCKWISE;
    case DUI_ICON_POWER: return flags & DUI_F_ON ? PH_TOGGLE_RIGHT : PH_TOGGLE_LEFT;
    case DUI_ICON_QR: return PH_QR_CODE;
    case DUI_ICON_LIST: return PH_LIST_BULLETS;
    case DUI_ICON_SUN_LOW: return PH_SUN_DIM;
    case DUI_ICON_SUN_HIGH: return PH_SUN;
    case DUI_ICON_SOUND: return PH_SPEAKER_HIGH;
    case DUI_ICON_MUTE: return PH_SPEAKER_SLASH;
    case DUI_ICON_LOCK: return PH_LOCK;
    case DUI_ICON_CLOSE: return PH_X;
    case DUI_ICON_DEL: return PH_BACKSPACE;
    case DUI_ICON_FLASH: return PH_LIGHTNING;
    case DUI_ICON_EDIT: return PH_PENCIL_SIMPLE;
    case DUI_ICON_CIRCUITRY: return PH_CIRCUITRY;
    case DUI_ICON_SEARCH: return PH_MAGNIFYING_GLASS;
    case DUI_ICON_COMPANION: return PH_WEBHOOKS_LOGO;
    default: return -1;
    }
}

void dui_icon(const dui_canvas_t *c, int icon, float cx, float cy, float size, uint32_t rgb, int8_t rssi, uint16_t flags)
{
    const ictx_t I = {c, cx, cy, size / 24};
    rs_shape_t s;
    const int g = ph_of(icon, flags);
    if (g >= 0) {
        glyph(&I, g, rgb, 1, NULL);
        return;
    }
    switch (icon) {
    case DUI_ICON_WIFI: wifi(&I, rgb, rssi); break;
    case DUI_ICON_NONET: { /* wifi-slash, its slash red (a problem): the glyph cleared under it, then the slash */
        const float r = PH_STROKE / 2.0f * ph_k(&I);
        const rs_capsule_t cut = {PX(&I, 48), PY(&I, 40), PX(&I, 208), PY(&I, 216), r};
        glyph(&I, PH_WIFI_SLASH, rgb, 1, &cut);
        slash(&I, SUI_RGB_BAD);
        break;
    }
    case DUI_ICON_WIFI_OFF: glyph(&I, PH_WIFI_SLASH, rgb, 1, NULL); break; /* off by choice, all one ink */
    case DUI_ICON_HOTSPOT_OFF: { /* broadcast crossed as wifi-slash is: cut clear on the slash's upper right to
                                  * wifi-slash's own clearance (PH_SLASH_CLEAR from its line), up to it on the lower left;
                                  * then the slash. The cut: a capsule across that band, its centreline moved off the
                                  * slash's toward the upper right */
        const float k = ph_k(&I), h = PH_STROKE / 2.0f, o = (PH_SLASH_CLEAR - h) / 2, r = (PH_SLASH_CLEAR + h) / 2 * k;
        const float nx = 0.7399f * o, ny = -0.6726f * o; /* the slash's normal, (176, -160) / |(160, 176)|, by o */
        const rs_capsule_t cut = {PX(&I, 48 + nx), PY(&I, 40 + ny), PX(&I, 208 + nx), PY(&I, 216 + ny), r};
        glyph(&I, PH_BROADCAST, rgb, 1, &cut);
        slash(&I, rgb);
        break;
    }
    case DUI_ICON_SAT: satellite(&s, &I, rgb); break;
    case DUI_ICON_LOGO: /* the web page's i-keys: dots r 2.6 at 5 / 12 / 19 of its 24-unit box */
        rs_init(&s, 0);
        for (int v = -7; v <= 7; v += 7)
            for (int u = -7; u <= 7; u += 7) disc(&s, &I, (float)u, (float)v, 2.6f);
        rs_draw(c, &s, rgb, 1);
        break;
    default: break;
    }
}
