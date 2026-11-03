/* Pure: the anti-aliased rasteriser (raster.h): signed distances, one coverage per pixel. */
#include "raster.h"

#include <math.h>
#include <string.h>

#define TAU_F 6.28318531f

enum { P_SEG, P_ARC, P_RING, P_DISC, P_ELLIPSE, P_RRECT, P_RRECT_FILL };

void rs_init(rs_shape_t *s, float stroke_w)
{
    memset(s, 0, sizeof *s);
    s->w = stroke_w;
    s->x0 = s->y0 = 1e9f;
    s->x1 = s->y1 = -1e9f;
}

static void grow(rs_shape_t *s, float x0, float y0, float x1, float y1)
{
    if (x0 < s->x0) s->x0 = x0;
    if (y0 < s->y0) s->y0 = y0;
    if (x1 > s->x1) s->x1 = x1;
    if (y1 > s->y1) s->y1 = y1;
}

/* A new prim of s; NULL past RS_PRIMS (no icon takes that many): the prim is left out. */
static rs_prim_t *add(rs_shape_t *s, int type)
{
    if (s->n >= RS_PRIMS) return NULL;
    rs_prim_t *p = &s->p[s->n++];
    memset(p, 0, sizeof *p);
    p->type = (uint8_t)type;
    return p;
}

static void set4(rs_prim_t *p, float a0, float a1, float a2, float a3)
{
    if (!p) return;
    p->a[0] = a0;
    p->a[1] = a1;
    p->a[2] = a2;
    p->a[3] = a3;
}

void rs_seg(rs_shape_t *s, float x0, float y0, float x1, float y1)
{
    set4(add(s, P_SEG), x0, y0, x1, y1);
    const float h = s->w / 2;
    grow(s, fminf(x0, x1) - h, fminf(y0, y1) - h, fmaxf(x0, x1) + h, fmaxf(y0, y1) + h);
}

/* An arc's prim: centre, radius, the direction of its middle and the cosine of half its span (a point
 * is within the arc's angles when its direction is that close to the middle), and its end points. All
 * the trigonometry is done here, once. */
static void arc_prim(rs_prim_t *p, float cx, float cy, float r, float a0, float a1)
{
    if (!p) return;
    float span = a1 - a0; /* canvas: clockwise from a0 to a1, at most a full turn */
    if (span < 0) span = fmodf(span, TAU_F) + TAU_F;
    if (span > TAU_F) span = TAU_F;
    const float mid = a0 + span / 2;
    p->type = P_ARC;
    set4(p, cx, cy, r, cosf(mid));
    p->a[4] = sinf(mid);
    p->a[5] = cosf(span / 2);
    p->a[6] = cx + r * cosf(a0);
    p->a[7] = cy + r * sinf(a0);
    p->a[8] = cx + r * cosf(a0 + span);
    p->a[9] = cy + r * sinf(a0 + span);
}

void rs_arc(rs_shape_t *s, float cx, float cy, float r, float a0, float a1)
{
    arc_prim(add(s, P_ARC), cx, cy, r, a0, a1);
    const float h = s->w / 2 + r;
    grow(s, cx - h, cy - h, cx + h, cy + h);
}

void rs_ring(rs_shape_t *s, float cx, float cy, float r)
{
    set4(add(s, P_RING), cx, cy, r, 0);
    const float h = s->w / 2 + r;
    grow(s, cx - h, cy - h, cx + h, cy + h);
}

void rs_disc(rs_shape_t *s, float cx, float cy, float r)
{
    set4(add(s, P_DISC), cx, cy, r, 0);
    grow(s, cx - r, cy - r, cx + r, cy + r);
}

void rs_ellipse(rs_shape_t *s, float cx, float cy, float rx, float ry)
{
    set4(add(s, P_ELLIPSE), cx, cy, rx, ry);
    const float h = s->w / 2;
    grow(s, cx - rx - h, cy - ry - h, cx + rx + h, cy + ry + h);
}

void rs_rrect(rs_shape_t *s, float x, float y, float w, float h, float r)
{
    rs_prim_t *p = add(s, P_RRECT);
    set4(p, x + w / 2, y + h / 2, w / 2, h / 2);
    if (p) p->a[4] = r;
    const float k = s->w / 2;
    grow(s, x - k, y - k, x + w + k, y + h + k);
}

void rs_rrect_fill(rs_shape_t *s, float x, float y, float w, float h, float r)
{
    rs_prim_t *p = add(s, P_RRECT_FILL);
    set4(p, x + w / 2, y + h / 2, w / 2, h / 2);
    if (p) p->a[4] = r;
    grow(s, x, y, x + w, y + h);
}

void rs_rect_fill(rs_shape_t *s, float x, float y, float w, float h) { rs_rrect_fill(s, x, y, w, h, 0); }

void rs_poly_fill(rs_shape_t *s, const float *xy, int n)
{
    if (n > 6) n = 6;
    s->npoly = n;
    memcpy(s->poly, xy, sizeof(float) * 2 * (size_t)n);
    for (int i = 0; i < n; i++) grow(s, xy[2 * i], xy[2 * i + 1], xy[2 * i], xy[2 * i + 1]);
}

/* The signed distance from (x, y) to a rounded box centred (cx, cy), half sizes hw, hh, radius r. */
static float box_d(float x, float y, float cx, float cy, float hw, float hh, float r)
{
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    const float qx = fabsf(x - cx) - (hw - r), qy = fabsf(y - cy) - (hh - r);
    const float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
}

static float seg_d(float x, float y, float ax, float ay, float bx, float by)
{
    const float px = x - ax, py = y - ay, ex = bx - ax, ey = by - ay, ee = ex * ex + ey * ey;
    float h = ee > 0 ? (px * ex + py * ey) / ee : 0;
    h = h < 0 ? 0 : h > 1 ? 1 : h;
    const float dx = px - ex * h, dy = py - ey * h;
    return sqrtf(dx * dx + dy * dy);
}

static float prim_d(const rs_prim_t *p, float x, float y, float hw)
{
    const float *a = p->a;
    switch (p->type) {
    case P_SEG: return seg_d(x, y, a[0], a[1], a[2], a[3]) - hw;
    case P_ARC: {
        const float dx = x - a[0], dy = y - a[1], len = sqrtf(dx * dx + dy * dy);
        if (len <= 0 || dx * a[3] + dy * a[4] >= a[5] * len) return fabsf(len - a[2]) - hw; /* within its angles */
        const float d0 = sqrtf((x - a[6]) * (x - a[6]) + (y - a[7]) * (y - a[7]));
        const float d1 = sqrtf((x - a[8]) * (x - a[8]) + (y - a[9]) * (y - a[9]));
        return fminf(d0, d1) - hw;
    }
    case P_RING: return fabsf(sqrtf((x - a[0]) * (x - a[0]) + (y - a[1]) * (y - a[1])) - a[2]) - hw;
    case P_DISC: return sqrtf((x - a[0]) * (x - a[0]) + (y - a[1]) * (y - a[1])) - a[2];
    case P_ELLIPSE: { /* the distance to the curve, to first order */
        const float u = (x - a[0]) / a[2], v = (y - a[1]) / a[3];
        const float k0 = sqrtf(u * u + v * v), k1 = sqrtf(u * u / (a[2] * a[2]) + v * v / (a[3] * a[3]));
        return k1 > 0 ? fabsf(k0 * (k0 - 1) / k1) - hw : a[2] - hw;
    }
    case P_RRECT: return fabsf(box_d(x, y, a[0], a[1], a[2], a[3], a[4])) - hw;
    case P_RRECT_FILL: return box_d(x, y, a[0], a[1], a[2], a[3], a[4]);
    default: return 1e9f;
    }
}

/* The signed distance to a polygon (even-odd inside). */
static float poly_d(const float *v, int n, float x, float y)
{
    float d = (x - v[0]) * (x - v[0]) + (y - v[1]) * (y - v[1]);
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float ex = v[2 * j] - v[2 * i], ey = v[2 * j + 1] - v[2 * i + 1];
        const float wx = x - v[2 * i], wy = y - v[2 * i + 1], ee = ex * ex + ey * ey;
        float h = ee > 0 ? (wx * ex + wy * ey) / ee : 0;
        h = h < 0 ? 0 : h > 1 ? 1 : h;
        const float bx = wx - ex * h, by = wy - ey * h;
        d = fminf(d, bx * bx + by * by);
        if ((v[2 * i + 1] > y) != (v[2 * j + 1] > y) &&
            x < (v[2 * j] - v[2 * i]) * (y - v[2 * i + 1]) / (v[2 * j + 1] - v[2 * i + 1]) + v[2 * i])
            in = !in;
    }
    return in ? -sqrtf(d) : sqrtf(d);
}

/* Draws the union of n prims (and the polygon), intersected with clip (may be NULL), over the box
 * [bx0, bx1] x [by0, by1] grown by a pixel and clamped to the canvas. */
static void draw_core(const dui_canvas_t *c, const rs_prim_t *p, int n, const float *poly, int npoly,
                      const rs_prim_t *clip, float hw, float bx0, float by0, float bx1, float by1, uint32_t rgb,
                      float alpha)
{
    if ((!n && !npoly) || alpha <= 0) return;
    const int x0 = (int)floorf(bx0 - 1) < 0 ? 0 : (int)floorf(bx0 - 1);
    const int y0 = (int)floorf(by0 - 1) < 0 ? 0 : (int)floorf(by0 - 1);
    const int x1 = (int)ceilf(bx1 + 1) > c->w ? c->w : (int)ceilf(bx1 + 1);
    const int y1 = (int)ceilf(by1 + 1) > c->h ? c->h : (int)ceilf(by1 + 1);
    const int cr = (int)(rgb >> 16 & 255), cg = (int)(rgb >> 8 & 255), cb = (int)(rgb & 255);
    for (int y = y0; y < y1; y++) {
        const float py = (float)y + 0.5f;
        for (int x = x0; x < x1; x++) {
            const float px = (float)x + 0.5f;
            float d = 1e9f;
            for (int i = 0; i < n; i++) d = fminf(d, prim_d(&p[i], px, py, hw));
            if (npoly) d = fminf(d, poly_d(poly, npoly, px, py));
            if (clip) d = fmaxf(d, prim_d(clip, px, py, 0));
            float cov = 0.5f - d;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            const int a = (int)(cov * alpha * 255 + 0.5f);
            if (a <= 0) continue;
            uint8_t *o = c->rgb + 3 * ((size_t)y * c->w + x);
            o[0] = (uint8_t)((o[0] * (255 - a) + cr * a + 127) / 255);
            o[1] = (uint8_t)((o[1] * (255 - a) + cg * a + 127) / 255);
            o[2] = (uint8_t)((o[2] * (255 - a) + cb * a + 127) / 255);
        }
    }
}

void rs_draw(const dui_canvas_t *c, const rs_shape_t *s, uint32_t rgb, float alpha)
{
    draw_core(c, s->p, s->n, s->poly, s->npoly, NULL, s->w / 2, s->x0, s->y0, s->x1, s->y1, rgb, alpha);
}

void rs_draw_disc(const dui_canvas_t *c, float cx, float cy, float r, uint32_t rgb, float alpha)
{
    rs_prim_t p = {P_DISC, {cx, cy, r}};
    draw_core(c, &p, 1, NULL, 0, NULL, 0, cx - r, cy - r, cx + r, cy + r, rgb, alpha);
}

void rs_draw_ring(const dui_canvas_t *c, float cx, float cy, float r, float stroke_w, uint32_t rgb)
{
    rs_prim_t p = {P_RING, {cx, cy, r}};
    const float h = r + stroke_w / 2;
    draw_core(c, &p, 1, NULL, 0, NULL, stroke_w / 2, cx - h, cy - h, cx + h, cy + h, rgb, 1);
}

void rs_draw_rrect(const dui_canvas_t *c, float x, float y, float w, float h, float r, float stroke_w, uint32_t rgb)
{
    rs_prim_t p = {(uint8_t)(stroke_w > 0 ? P_RRECT : P_RRECT_FILL), {x + w / 2, y + h / 2, w / 2, h / 2, r}};
    const float k = stroke_w / 2;
    draw_core(c, &p, 1, NULL, 0, NULL, k, x - k, y - k, x + w + k, y + h + k, rgb, 1);
}

void rs_draw_rect_clipped(const dui_canvas_t *c, float x, float y, float w, float h, float cx, float cy, float cw,
                          float ch, float cr, uint32_t rgb)
{
    rs_prim_t p = {P_RRECT_FILL, {x + w / 2, y + h / 2, w / 2, h / 2, 0}};
    rs_prim_t clip = {P_RRECT_FILL, {cx + cw / 2, cy + ch / 2, cw / 2, ch / 2, cr}};
    draw_core(c, &p, 1, NULL, 0, &clip, 0, x, y, x + w, y + h, rgb, 1);
}

void rs_draw_segs(const dui_canvas_t *c, const float *xy, int n, float stroke_w, uint32_t rgb)
{
    rs_prim_t p[4];
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) {
        const float *s = xy + 4 * i;
        p[i] = (rs_prim_t){P_SEG, {s[0], s[1], s[2], s[3]}};
        x0 = fminf(x0, fminf(s[0], s[2]));
        y0 = fminf(y0, fminf(s[1], s[3]));
        x1 = fmaxf(x1, fmaxf(s[0], s[2]));
        y1 = fmaxf(y1, fmaxf(s[1], s[3]));
    }
    const float k = stroke_w / 2;
    draw_core(c, p, n, NULL, 0, NULL, k, x0 - k, y0 - k, x1 + k, y1 + k, rgb, 1);
}

void rs_fill(const dui_canvas_t *c, int x, int y, int w, int h, uint32_t rgb)
{
    const uint8_t col[3] = {(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb};
    const int xa = x < 0 ? 0 : x, ya = y < 0 ? 0 : y;
    const int xb = x + w > c->w ? c->w : x + w, yb = y + h > c->h ? c->h : y + h;
    if (xa >= xb || ya >= yb) return;
    /* the first row pixel by pixel, then that span copied to the others (the same bytes) */
    uint8_t *row0 = c->rgb + 3 * ((size_t)ya * c->w + xa);
    for (int xx = 0; xx < xb - xa; xx++) memcpy(row0 + 3 * xx, col, 3);
    const size_t span = 3 * (size_t)(xb - xa);
    for (int yy = ya + 1; yy < yb; yy++) memcpy(c->rgb + 3 * ((size_t)yy * c->w + xa), row0, span);
}

/* ---- Contours, nonzero, by exact area (font-rs's accumulation, a row at a time) ---- */

/* Edge (x0, y0) -> (x1, y1) (px, x from the row buffer's left) into row ry's accumulation acc[0 .. w + 1]: the
 * signed area it covers to the right of it in each cell (its direction the sign: down +, up -), so that the
 * running sum along the row is each pixel's winding-weighted coverage. */
static void acc_edge(float *acc, int w, int ry, float x0, float y0, float x1, float y1)
{
    if (y0 == y1) return;
    float dir = 1;
    if (y0 > y1) { /* top to bottom */
        float t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
        dir = -1;
    }
    const float top = fmaxf(y0, (float)ry), bot = fminf(y1, (float)ry + 1);
    if (top >= bot) return;
    const float dxdy = (x1 - x0) / (y1 - y0);
    const float xa = x0 + (top - y0) * dxdy, xb = x0 + (bot - y0) * dxdy;
    const float d = (bot - top) * dir;
    float lo = fminf(xa, xb), hi = fmaxf(xa, xb);
    if (lo < 0) lo = 0;
    if (hi < 0) hi = 0; /* never a cell before acc[0] (x is measured from the leftmost point's floor: kept anyway) */
    if (hi > (float)w) hi = (float)w;
    if (lo > hi) lo = hi;
    const float lof = floorf(lo);
    const int i0 = (int)lof, i1 = (int)ceilf(hi);
    if (i1 <= i0 + 1) { /* within one cell: its share of the cell, the rest to the next */
        const float xm = 0.5f * (lo + hi) - lof;
        acc[i0] += d - d * xm;
        acc[i0 + 1] += d * xm;
        return;
    }
    const float s = 1.0f / (hi - lo), f0 = lo - lof, f1 = hi - (float)i1 + 1;
    const float a0 = 0.5f * s * (1 - f0) * (1 - f0), am = 0.5f * s * f1 * f1;
    acc[i0] += d * a0;
    if (i1 == i0 + 2) {
        acc[i0 + 1] += d * (1 - a0 - am);
    } else {
        const float a1 = s * (1.5f - f0);
        acc[i0 + 1] += d * (a1 - a0);
        for (int i = i0 + 2; i < i1 - 1; i++) acc[i] += d * s;
        const float a2 = a1 + (float)(i1 - i0 - 3) * s;
        acc[i1 - 1] += d * (1 - a2 - am);
    }
    acc[i1] += d * am;
}

void rs_fill_contours(const dui_canvas_t *c, const int16_t *pts, const uint16_t *ends, int c0, int nc, float ox,
                      float oy, float k, const rs_capsule_t *cut, uint32_t rgb, float alpha)
{
    if (nc <= 0 || alpha <= 0) return;
    const int pfirst = c0 > 0 ? ends[c0 - 1] : 0, plast = ends[c0 + nc - 1];
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    for (int i = pfirst; i < plast; i++) {
        const float x = ox + pts[2 * i] * k, y = oy + pts[2 * i + 1] * k;
        bx0 = fminf(bx0, x), by0 = fminf(by0, y), bx1 = fmaxf(bx1, x), by1 = fmaxf(by1, y);
    }
    const float left = floorf(bx0);
    int w = (int)ceilf(bx1) - (int)left + 1;
    if (w > RS_FILL_MAX_W) w = RS_FILL_MAX_W;
    const int ya = (int)floorf(by0) < 0 ? 0 : (int)floorf(by0), yb = (int)ceilf(by1) > c->h ? c->h : (int)ceilf(by1);
    const int cr = (int)(rgb >> 16 & 255), cg = (int)(rgb >> 8 & 255), cb = (int)(rgb & 255);
    float acc[RS_FILL_MAX_W + 2];
    for (int ry = ya; ry < yb; ry++) {
        memset(acc, 0, sizeof(float) * (size_t)(w + 2));
        for (int ci = c0; ci < c0 + nc; ci++) {
            const int p0 = ci > 0 ? ends[ci - 1] : 0, p1 = ends[ci];
            float px = ox + pts[2 * (p1 - 1)] * k - left, py = oy + pts[2 * (p1 - 1) + 1] * k; /* closed: last to first */
            for (int i = p0; i < p1; i++) {
                const float x = ox + pts[2 * i] * k - left, y = oy + pts[2 * i + 1] * k;
                if (!((py < ry && y < ry) || (py >= ry + 1 && y >= ry + 1))) acc_edge(acc, w, ry, px, py, x, y);
                px = x, py = y;
            }
        }
        float run = 0;
        const float cy = (float)ry + 0.5f;
        for (int i = 0; i < w; i++) {
            run += acc[i];
            float cov = fabsf(run);
            if (cov > 1) cov = 1;
            const int x = (int)left + i;
            if (cov <= 0.002f || x < 0 || x >= c->w) continue;
            if (cut) { /* the slash's capsule, and its edge antialiased */
                const float dc = seg_d((float)x + 0.5f, cy, cut->x0, cut->y0, cut->x1, cut->y1) - cut->r;
                if (dc <= -0.5f) continue;
                if (dc < 0.5f) cov *= dc + 0.5f;
            }
            const int a = (int)(cov * alpha * 255 + 0.5f);
            if (a <= 0) continue;
            uint8_t *o = c->rgb + 3 * ((size_t)ry * c->w + x);
            o[0] = (uint8_t)((o[0] * (255 - a) + cr * a + 127) / 255);
            o[1] = (uint8_t)((o[1] * (255 - a) + cg * a + 127) / 255);
            o[2] = (uint8_t)((o[2] * (255 - a) + cb * a + 127) / 255);
        }
    }
}
