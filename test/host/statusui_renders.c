/* Host tool: PNG renders of what statusui still draws on the decks (custom messages and the "Reconnecting"
 * overlay; the maintenance screens are deckui's: deckui_renders), one file per model and screen (its
 * pages one above the other), for looking at the word-per-key layout without a deck.
 *   build-host/statusui_renders <out dir>
 * Not a test (ctest doesn't run it); tools/test-host.sh builds it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "deckmodel/model.h"
#include "statusui/statusui.h"

#define GAP 12       /* px between keys, and around them */
#define PAGE_GAP 28  /* px between pages */
#define BODY 0x26    /* the deck's body colour (grey) */
#define DIM_PCT 40   /* "Reconnecting" is drawn over Companion's images dimmed this much (surfaces.c) */

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
        raw[row * (size_t)y] = 0; /* filter: none */
        memcpy(raw + row * (size_t)y + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    int rc = compress2(z, &z_len, raw, (uLong)raw_len, 9) == Z_OK ? 0 : -1;
    FILE *f = rc == 0 ? fopen(path, "wb") : NULL;
    if (f) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        uint8_t ihdr[13];
        be32(ihdr, (uint32_t)w);
        be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;  /* bits per channel */
        ihdr[9] = 2;  /* RGB */
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

/* A stand-in for Companion's image on a key, dimmed as in the "Reconnecting" phase. */
static void fake_dimmed_image(uint8_t *key, int px, int col, int row)
{
    for (int y = 0; y < px; y++)
        for (int x = 0; x < px; x++) {
            uint8_t *o = key + 3 * (y * px + x);
            o[0] = (uint8_t)(255 * x / px);
            o[1] = (uint8_t)(255 * y / px);
            o[2] = (uint8_t)(((x / 12 + y / 12 + col + row) & 1) ? 230 : 70);
            for (int k = 0; k < 3; k++) o[k] = (uint8_t)(o[k] * DIM_PCT / 100);
        }
}

/* Every page of c on model m, pages one above the other; overlay: drawn over dimmed images. */
static int render(const char *dir, const char *model, const deck_model_t *m, const char *screen,
                  const sui_content_t *c, int overlay)
{
    const int px = m->key_px, pages = sui_page_count(m->cols, m->rows, px, c);
    const int dw = GAP + m->cols * (px + GAP), dh = GAP + m->rows * (px + GAP);
    const int w = dw, h = pages * dh + (pages - 1) * PAGE_GAP;
    uint8_t *img = malloc((size_t)w * h * 3), *key = malloc((size_t)px * px * 3);
    if (!img || !key) return -1;
    memset(img, 0xff, (size_t)w * h * 3); /* the gap between pages: white */
    for (int p = 0; p < pages; p++) {
        const int oy = p * (dh + PAGE_GAP);
        memset(img + (size_t)oy * w * 3, BODY, (size_t)dh * w * 3);
        for (int r = 0; r < m->rows; r++)
            for (int k = 0; k < m->cols; k++) {
                if (overlay) {
                    fake_dimmed_image(key, px, k, r);
                    sui_render_overlay_key(px, c, key); /* the same on every key, as surfaces draws it */
                } else {
                    sui_render_key(m->cols, m->rows, px, c, p, k, r, key);
                }
                const int x0 = GAP + k * (px + GAP), y0 = oy + GAP + r * (px + GAP);
                for (int y = 0; y < px; y++)
                    memcpy(img + ((size_t)(y0 + y) * w + x0) * 3, key + (size_t)y * px * 3, (size_t)px * 3);
            }
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%s.png", dir, model, screen);
    const int rc = write_png(path, w, h, img);
    if (rc == 0) printf("%s (%d page%s)\n", path, pages, pages == 1 ? "" : "s");
    free(img);
    free(key);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <out dir>\n", argv[0]);
        return 2;
    }
    static const struct { const char *name; uint16_t pid; } M[] = {
        {"mini-3x2-80", 0x0063},     {"mk2-5x3-72", 0x0080}, {"xl-8x4-96", 0x006C},
        {"module32-8x4-96", 0x00BA}, {"neo-4x2-96", 0x009A}, {"plus-4x2-120", 0x0084},
        {"plusxl-9x4-112", 0x00C6},
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) {
        const deck_model_t *m = deck_model_find(DECK_VID_ELGATO, M[i].pid);
        if (!m) return 1;
        sui_content_t c;
        const char *n = M[i].name;
        sui_screen_reconnecting(&c, NULL);
        fails += render(argv[1], n, m, "reconnecting-overlay", &c, 1) != 0;
        sui_screen_message(&c, "Back in 5 minutes");
        fails += render(argv[1], n, m, "message", &c, 0) != 0;
        sui_screen_message(&c, "Hello\n\nWorld");
        fails += render(argv[1], n, m, "message-empty-line", &c, 0) != 0;
        sui_screen_message(&c, "Rehearsal at 14:00 in studio B\nPlease keep the decks free\n\n"
                               "Questions: ask Tech-Desk at 10.0.0.12");
        fails += render(argv[1], n, m, "message-long", &c, 0) != 0;
    }
    return fails ? 1 : 0;
}
