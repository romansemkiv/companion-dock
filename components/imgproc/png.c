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
    if (err || iw != (unsigned)w || ih != (unsigned)h) {
        lodepng_state_cleanup(&st);
        return false;
    }

    /* What the data may inflate to: no image of this size has more scanline bytes than 16-bit RGBA (8 a pixel) with a
     * filter byte per row, and Adam7 interlacing has under 2 h + 4 rows in its seven passes. Data that inflates past
     * it (a few bytes that would grow to gigabytes) is refused there, not when the memory runs out. */
    st.decoder.zlibsettings.max_output_size = (size_t)h * ((size_t)w * 8 + 2) + 4;
    st.info_raw.colortype = LCT_RGBA; /* any colour type and depth -> RGBA8; CRCs and Adler checked */
    st.info_raw.bitdepth = 8;
    unsigned char *rgba = NULL;
    err = lodepng_decode(&rgba, &iw, &ih, &st, png, len);
    lodepng_state_cleanup(&st);
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
