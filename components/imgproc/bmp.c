#include "imgproc/imgproc.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

size_t img_bmp_pack(const uint8_t *rgb, int w, int h, uint8_t *out, size_t cap)
{
    if (w <= 0 || h <= 0) return 0;
    size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
    size_t total = 54 + stride * (size_t)h;
    if (cap < total) return 0;

    for (int i = 0; i < 54; i++) out[i] = 0;
    out[0] = 'B'; out[1] = 'M';
    put32(out + 2, (uint32_t)total);
    put32(out + 10, 54);
    put32(out + 14, 40);
    put32(out + 18, (uint32_t)w);
    put32(out + 22, (uint32_t)h);
    put16(out + 26, 1);
    put16(out + 28, 24);
    put32(out + 34, (uint32_t)(stride * (size_t)h));
    put32(out + 38, 2835);
    put32(out + 42, 2835);

    uint8_t *row = out + 54;
    for (int y = 0; y < h; y++, row += stride) {
        const uint8_t *s = rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            row[x * 3] = s[x * 3 + 2];
            row[x * 3 + 1] = s[x * 3 + 1];
            row[x * 3 + 2] = s[x * 3];
        }
        for (size_t p = (size_t)w * 3; p < stride; p++) row[p] = 0;
    }
    return total;
}

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

bool img_bmp_unpack(const uint8_t *bmp, size_t len, int w, int h, uint8_t *rgb)
{
    if (!bmp || !rgb || w <= 0 || h <= 0 || len < 54 || bmp[0] != 'B' || bmp[1] != 'M') return false;
    const uint32_t off = get32(bmp + 10), hdr = get32(bmp + 14);
    const int32_t bw = (int32_t)get32(bmp + 18), bh = (int32_t)get32(bmp + 22);
    if (hdr < 40 || bw != w || (bh != h && bh != -h) || get16(bmp + 26) != 1 || get16(bmp + 28) != 24 ||
        get32(bmp + 30) != 0)
        return false;
    const size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
    if (off < 54 || off > len || stride * (size_t)h > len - off) return false;
    const uint8_t *row = bmp + off;
    for (int y = 0; y < h; y++, row += stride) {
        uint8_t *d = rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            d[x * 3] = row[x * 3 + 2];
            d[x * 3 + 1] = row[x * 3 + 1];
            d[x * 3 + 2] = row[x * 3];
        }
    }
    return true;
}
