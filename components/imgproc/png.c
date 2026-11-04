#include "imgproc/png.h"

#include <stdlib.h>

#include "lodepng.h"

/* The largest image decoded: Satellite's largest bitmap is a 1200 x 100 strip (the Plus XL's), far under this. */
#define PNG_MAX_PIXELS (1024u * 1024u)

bool img_png_decode_rgb(const uint8_t *png, size_t len, int w, int h, uint8_t *rgb)
{
    if (!png || !len || !rgb || w <= 0 || h <= 0 || (unsigned)w * (unsigned)h > PNG_MAX_PIXELS) return false;

    /* the header first: an image of another size is refused before any memory is taken for it */
    LodePNGState st;
    lodepng_state_init(&st);
    unsigned iw = 0, ih = 0;
    unsigned err = lodepng_inspect(&iw, &ih, &st, png, len);
    lodepng_state_cleanup(&st);
    if (err || iw != (unsigned)w || ih != (unsigned)h) return false;

    unsigned char *rgba = NULL;
    err = lodepng_decode32(&rgba, &iw, &ih, png, len); /* any colour type and depth -> RGBA8; CRCs and Adler checked */
    if (err || !rgba || iw != (unsigned)w || ih != (unsigned)h) {
        free(rgba);
        return false;
    }
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        const unsigned a = rgba[i * 4 + 3];
        for (int c = 0; c < 3; c++) rgb[i * 3 + c] = (uint8_t)((rgba[i * 4 + c] * a + 127u) / 255u);
    }
    free(rgba);
    return true;
}
