/* Output of tools/phosphor_icons.py: do not edit (regenerate it from the SVGs named below).
 *
 * Phosphor Icons (https://phosphoricons.com), @phosphor-icons/core 2.1.1, Regular weight. MIT License:
 *
 * Copyright (c) 2023 Phosphor Icons
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of
 * the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once
#include <stdint.h>

#define PH_UNIT 16 /* the points' fixed point: per unit of Phosphor's 256-unit box */
#define PH_BOX 256
#define PH_WEIGHT "regular"
#define PH_STROKE 16 /* the weight's stroke, in units of the box */
#define PH_SLASH_CLEAR 23.84f /* wifi-slash: its glyph clear of the slash's centreline on its upper right */

enum {
    PH_WIFI_HIGH, /* wifi-high */
    PH_WIFI_MEDIUM, /* wifi-medium */
    PH_WIFI_LOW, /* wifi-low */
    PH_NETWORK, /* network */
    PH_GLOBE, /* globe */
    PH_SQUARES_FOUR, /* squares-four */
    PH_HARD_DRIVE, /* hard-drive */
    PH_BROADCAST, /* broadcast */
    PH_INFO, /* info */
    PH_CARET_LEFT, /* caret-left */
    PH_CARET_RIGHT, /* caret-right */
    PH_LIST, /* list */
    PH_ARROW_CLOCKWISE, /* arrow-clockwise */
    PH_POWER, /* power */
    PH_ARROWS_CLOCKWISE, /* arrows-clockwise */
    PH_TOGGLE_LEFT, /* toggle-left */
    PH_TOGGLE_RIGHT, /* toggle-right */
    PH_QR_CODE, /* qr-code */
    PH_LIST_BULLETS, /* list-bullets */
    PH_SUN_DIM, /* sun-dim */
    PH_SUN, /* sun */
    PH_SPEAKER_HIGH, /* speaker-high */
    PH_SPEAKER_SLASH, /* speaker-slash */
    PH_LOCK, /* lock */
    PH_X, /* x */
    PH_BACKSPACE, /* backspace */
    PH_LIGHTNING, /* lightning */
    PH_PENCIL_SIMPLE, /* pencil-simple */
    PH_CIRCUITRY, /* circuitry */
    PH_MAGNIFYING_GLASS, /* magnifying-glass */
    PH_WIFI_SLASH, /* wifi-slash */
    PH_WEBHOOKS_LOGO, /* webhooks-logo */
    PH_COUNT
};

typedef struct {
    uint16_t c0, nc; /* its contours: ph_end[c0 .. c0 + nc - 1] */
} ph_glyph_t;

/* The points (x, y pairs; contour k's points are ph_end[k - 1] .. ph_end[k] - 1, ph_end[-1] being 0), each
 * contour closed (its last point joins its first), filled with the nonzero rule. */
extern const int16_t ph_pts[];
extern const uint16_t ph_end[];
extern const ph_glyph_t ph_glyph[PH_COUNT];
#define PH_NPTS 4580
#define PH_NCONTOURS 135
