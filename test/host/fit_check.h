#pragma once
/* The fits-in-the-key check: every text a key draws, descenders included, stays inside
 * the key's margins (SUI_MARGIN), and a pressable key's (a button, a pad key, the pager) inside its outline.
 * dui__text reports each text's ink box through dui__text_probe; the key is drawn for real. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../components/deckui/dui_int.h"
#include "deckui/deckui.h"
#include "statusui/statusui.h"
#include "unity.h"

static int fit_x0, fit_y0, fit_x1, fit_y1; /* the room the ink must stay in */
static int fit_bad;                       /* boxes outside it */
static char fit_where[96];

static void fit_probe(int px, int x0, int y0, int x1, int y1)
{
    (void)px;
    if (x0 >= fit_x0 && y0 >= fit_y0 && x1 <= fit_x1 && y1 <= fit_y1) return;
    if (!fit_bad++)
        printf("  ink (%d,%d)-(%d,%d) outside (%d,%d)-(%d,%d): %s\n", x0, y0, x1, y1, fit_x0, fit_y0, fit_x1, fit_y1,
               fit_where);
}

/* Key t (from dui_layout_key, decorated as shown) drawn on a key of key_px: fails with where when its ink
 * crosses the margins or, pressable, its outline (the body's inset U(5) plus the stroke U(2)). */
static void fit_key(const dui_tile_t *t, int key_px, const char *where)
{
    static uint8_t rgb[128 * 128 * 3];
    const bool pressable = t->kind == DUI_TILE_BUTTON || t->kind == DUI_TILE_KEY || t->kind == DUI_TILE_PAGER;
    const int in = pressable ? (int)((5.0f + 2.0f) * key_px / 96.0f) : SUI_MARGIN;
    fit_x0 = fit_y0 = in;
    fit_x1 = fit_y1 = key_px - in;
    fit_bad = 0;
    snprintf(fit_where, sizeof fit_where, "%s", where);
    dui__text_probe = fit_probe;
    dui_render_key(t, key_px, rgb);
    dui__text_probe = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(0, fit_bad, where);
}

/* A button's label (or hint) never runs into its icon: the icon's lowest ink row, from the key
 * drawn with and without its icon (the label as ever: the icon's place depends on it), lies above every text
 * box under the top band (the HOLD tag, the lock and a tag live there). */
static int fit_nbox, fit_box_y0[8];
static void fit_box_probe(int px, int x0, int y0, int x1, int y1)
{
    (void)px, (void)x0, (void)x1, (void)y1;
    if (fit_nbox < 8) fit_box_y0[fit_nbox++] = y0;
}

static void fit_icon_clear(const dui_tile_t *t0, int key_px, const char *where)
{
    if (t0->kind != DUI_TILE_BUTTON || (t0->flags & DUI_F_NAV) || !t0->icon ||
        (t0->caption && t0->caption[0] && !(t0->flags & DUI_F_TAG)))
        return;
    static uint8_t a[128 * 128 * 3], b[128 * 128 * 3];
    dui_tile_t t = *t0;
    fit_nbox = 0;
    dui__text_probe = fit_box_probe;
    dui_render_key(&t, key_px, a); /* as shown */
    dui__text_probe = NULL;
    t.icon = DUI_ICON_NONE;
    dui_render_key(&t, key_px, b); /* without its icon */
    int bottom = -1;
    for (int y = 0; y < key_px; y++)
        for (int x = 0; x < key_px && bottom < y; x++)
            if (memcmp(a + 3 * (y * key_px + x), b + 3 * (y * key_px + x), 3)) bottom = y;
    for (int i = 0; i < fit_nbox; i++)
        if (fit_box_y0[i] > key_px * 30 / 96) TEST_ASSERT_GREATER_THAN_MESSAGE(bottom, fit_box_y0[i], where);
}

/* Every key of every page of L on deck d. */
static void fit_layout(const dui_deck_t *d, const dui_layout_t *L, const char *name)
{
    for (int p = 0; p < L->pages; p++)
        for (int r = 0; r < d->rows; r++)
            for (int c = 0; c < d->cols; c++) {
                dui_tile_t t;
                if (!dui_layout_key(L, p, c, r, &t)) continue;
                char where[96];
                snprintf(where, sizeof where, "%dpx %s p%d (%d,%d) %.24s", d->key_px, name, p, c, r, t.text ? t.text : "");
                fit_key(&t, d->key_px, where);
                fit_icon_clear(&t, d->key_px, where);
                if (t.kind == DUI_TILE_BUTTON && t.hold_ms) { /* its hint too, as when let go early: inside, and */
                    t.flags |= DUI_F_HINT;                    /* at the label floor (14 px) */
                    fit_key(&t, d->key_px, where);
                    fit_icon_clear(&t, d->key_px, where);
                    char h[48];
                    const int n = snprintf(h, sizeof h, "Hold \xC2\xB7 %s", t.hint ? t.hint : "");
                    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(DUI_LABEL_MIN_PX, dui_label_px(h, t.hint && t.hint[0] ? n : 4, d->key_px), where);
                }
            }
}

/* The smallest text size a drawing used (bars: the floors). */
static int fit_min_px;
static void min_probe(int px, int x0, int y0, int x1, int y1)
{
    (void)x0, (void)y0, (void)x1, (void)y1;
    if (!fit_min_px || px < fit_min_px) fit_min_px = px;
}
