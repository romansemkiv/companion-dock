/* Pure: deck input events -> Companion control actions. Control positions follow
 * deck_model_controls (checked against it by the host tests). */
#include <stdlib.h>
#include "surfaces/surfaces.h"

/* Swipe angle thresholds as tangents: atan(|dy/dx|) >= 50 deg  <=>  |dy| >= tan(50 deg)*|dx|. */
#define TAN_50 1.19175359259421
#define TAN_22_5 0.414213562373095

/* 0..99 in decimal at p; returns the end */
static char *put_num(char *p, int v)
{
    if (v >= 10) *p++ = (char)('0' + v / 10);
    *p++ = (char)('0' + v % 10);
    return p;
}

/* "row/col" by hand, no snprintf: this runs on the 4 KB deck task, where newlib's formatter alone
 * takes 1152 B of stack. Every model's rows and columns are below 100: "99/99" fits. */
static void set_id(surf_action_t *a, int row, int col)
{
    _Static_assert(sizeof a->control_id >= sizeof "99/99", "control_id holds \"99/99\"");
    if (row < 0 || row > 99 || col < 0 || col > 99) {
        a->control_id[0] = '\0';
        return;
    }
    char *p = put_num(a->control_id, row);
    *p++ = '/';
    *put_num(p, col) = '\0';
}

/* Keys (left to right per row; Original v1 reports each row mirrored), pedals, and the Neo's
 * two touch keys under the outer key columns. */
static bool key_pos(const deck_model_t *m, uint8_t hw, int *row, int *col)
{
    const int nkeys = m->cols * m->rows;
    if (m->cols == 0) return false;
    if (hw < nkeys) {
        int c = hw % m->cols;
        *row = hw / m->cols;
        *col = m->rtl ? m->cols - 1 - c : c;
        return true;
    }
    if (m->touch_keys && hw < nkeys + 2) {
        *row = m->rows;
        *col = hw == nkeys ? 0 : m->cols - 1;
        return true;
    }
    return false;
}

static bool has_strip(const deck_model_t *m) { return m->lcd_w && m->encoders && m->enc_cols; }

/* LCD cell under x: floor(x / lcd_w * ncells), clamped to the last cell. */
static int strip_cell(const deck_model_t *m, uint16_t x)
{
    int cell = (int)((uint32_t)x * m->encoders / m->lcd_w);
    return cell < m->encoders ? cell : m->encoders - 1;
}

static int swipe(const deck_model_t *m, const deck_event_t *ev, surf_action_t *a)
{
    const long dx = (long)ev->x2 - ev->x, dy = (long)ev->y2 - ev->y;
    const double adx = (double)labs(dx), ady = (double)labs(dy);
    if (dx == 0 && dy == 0) return 0;
    if (ady >= TAN_50 * adx) { /* vertical: rotate the cell it stayed in */
        int cell = strip_cell(m, ev->x);
        if (cell != strip_cell(m, ev->x2)) return 0;
        a->kind = ACT_ROTATE;
        set_id(a, m->lcd_row, m->enc_cols[cell]);
        a->delta = ev->y > ev->y2 ? 1 : -1;
        return 1;
    }
    if (ady <= TAN_22_5 * adx) { /* horizontal: page change, swipe left = forward */
        a->kind = ACT_PAGE;
        a->control_id[0] = '\0';
        a->forward = ev->x > ev->x2;
        return 1;
    }
    return 0;
}

int inputmap_translate(const deck_model_t *m, const deck_event_t *ev, surf_action_t *out, int max)
{
    if (!m || !ev || !out || max < 1) return 0;
    surf_action_t *a = out;
    *a = (surf_action_t){0};
    int row, col;

    switch (ev->kind) {
    case DECK_EV_KEY:
        if (!key_pos(m, ev->index, &row, &col)) return 0;
        a->kind = ev->pressed ? ACT_PRESS : ACT_RELEASE;
        set_id(a, row, col);
        return 1;
    case DECK_EV_ENC_PRESS:
    case DECK_EV_ENC_ROTATE:
        if (!m->enc_cols || ev->index >= m->encoders) return 0;
        set_id(a, m->lcd_row + 1, m->enc_cols[ev->index]);
        if (ev->kind == DECK_EV_ENC_PRESS) {
            a->kind = ev->pressed ? ACT_PRESS : ACT_RELEASE;
        } else {
            if (ev->delta == 0) return 0;
            a->kind = ACT_ROTATE;
            a->delta = ev->delta;
        }
        return 1;
    case DECK_EV_TOUCH_TAP:
    case DECK_EV_TOUCH_LONG:
        if (!has_strip(m)) return 0;
        a->kind = ACT_TAP;
        set_id(a, m->lcd_row, m->enc_cols[strip_cell(m, ev->x)]);
        return 1;
    case DECK_EV_TOUCH_SWIPE:
        return has_strip(m) ? swipe(m, ev, a) : 0;
    default:
        return 0;
    }
}

bool inputmap_key_down(const deck_event_t *ev)
{
    if (!ev) return false;
    switch (ev->kind) {
    case DECK_EV_KEY:
    case DECK_EV_ENC_PRESS: return ev->pressed;
    case DECK_EV_TOUCH_TAP:
    case DECK_EV_TOUCH_LONG: return true;
    default: return false;
    }
}
