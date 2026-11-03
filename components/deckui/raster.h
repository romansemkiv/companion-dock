#pragma once
/* Private to deckui: a small anti-aliased rasteriser. A shape is a union of primitives (strokes of
 * one width with round caps and joins, and fills), drawn at once in one colour: each pixel's coverage
 * comes from its centre's signed distance to the shape (clamp(0.5 - d, 0, 1)), so joins are seamless
 * and edges smooth at any size. Coordinates in px, y down; angles in radians, clockwise from +x, an arc
 * running from a0 to a1 the way the clock turns (as canvas arc()). The rs_draw_* helpers draw one
 * primitive (or a few segments) without a shape, for callers that keep their stack small. */
#include <stdbool.h>
#include <stdint.h>

#include "deckui/deckui.h"

#define RS_PRIMS 12 /* the most any icon takes: the sun's disc and 8 rays; Lucide's satellite */

typedef struct {
    uint8_t type;
    float a[10];
} rs_prim_t;

typedef struct {
    float w;              // stroke width
    int n;
    rs_prim_t p[RS_PRIMS];
    int npoly;            // one filled polygon, at most 6 vertices
    float poly[12];
    float x0, y0, x1, y1; // the bounding box so far
} rs_shape_t;

void rs_init(rs_shape_t *s, float stroke_w);
void rs_seg(rs_shape_t *s, float x0, float y0, float x1, float y1);
void rs_arc(rs_shape_t *s, float cx, float cy, float r, float a0, float a1);
void rs_ring(rs_shape_t *s, float cx, float cy, float r);
void rs_disc(rs_shape_t *s, float cx, float cy, float r);
void rs_ellipse(rs_shape_t *s, float cx, float cy, float rx, float ry); // a stroke
void rs_rrect(rs_shape_t *s, float x, float y, float w, float h, float r); // a stroke
void rs_rrect_fill(rs_shape_t *s, float x, float y, float w, float h, float r);
void rs_rect_fill(rs_shape_t *s, float x, float y, float w, float h);
void rs_poly_fill(rs_shape_t *s, const float *xy, int n);
// Draws s in rgb (0xRRGGBB) at alpha (0..1), blended over what is there, clipped to the canvas.
void rs_draw(const dui_canvas_t *c, const rs_shape_t *s, uint32_t rgb, float alpha);

// One primitive, drawn straight away:
void rs_draw_disc(const dui_canvas_t *c, float cx, float cy, float r, uint32_t rgb, float alpha);
void rs_draw_ring(const dui_canvas_t *c, float cx, float cy, float r, float stroke_w, uint32_t rgb);
// A rounded rect: its outline stroke_w wide, or filled when stroke_w is 0.
void rs_draw_rrect(const dui_canvas_t *c, float x, float y, float w, float h, float r, float stroke_w, uint32_t rgb);
// The rect [x, x + w) x [y, y + h) filled, clipped to the rounded rect (cx, cy, cw, ch, cr).
void rs_draw_rect_clipped(const dui_canvas_t *c, float x, float y, float w, float h, float cx, float cy, float cw,
                          float ch, float cr, uint32_t rgb);
// n (at most 4) segments, xy holding x0, y0, x1, y1 each, as one stroke stroke_w wide.
void rs_draw_segs(const dui_canvas_t *c, const float *xy, int n, float stroke_w, uint32_t rgb);
// Fills the canvas's rect [x, x + w) x [y, y + h) (whole px) with rgb.
void rs_fill(const dui_canvas_t *c, int x, int y, int w, int h, uint32_t rgb);

// closed contours filled with the nonzero rule (Phosphor's outlines, their holes wound the other way),
// antialiased by each pixel's exact covered area. Contour k's points are pts[2 * ends[k - 1]] ..
// pts[2 * ends[k] - 1] (x, y in fixed point; ends[-1] being 0), each closed (the last point joins the first);
// contours c0 .. c0 + nc - 1 are drawn, a point (x, y) at (ox + x * k, oy + y * k) px, in rgb at alpha.
// cut (may be NULL): a capsule (x0, y0, x1, y1, its radius r, px) the fill is cleared from, antialiased (a
// sign's slash, the glyph cut clear around it). Rows wider than RS_FILL_MAX_W px are cut to it.
#define RS_FILL_MAX_W 256
typedef struct {
    float x0, y0, x1, y1, r;
} rs_capsule_t;
void rs_fill_contours(const dui_canvas_t *c, const int16_t *pts, const uint16_t *ends, int c0, int nc, float ox,
                      float oy, float k, const rs_capsule_t *cut, uint32_t rgb, float alpha);
