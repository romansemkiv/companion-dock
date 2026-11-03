/* Pure: what the deck's two pads share (dui_int.h): the PIN pad (pin.c) and the static-address editor
 * (ipedit.c) lay their keys out by hand on the same grid, with the same keys. */
#include <string.h>

#include "dui_int.h"

const char *const dui__digit[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};

void dui__lay_init(dui_layout_t *L, const dui_deck_t *d)
{
    memset(L, 0, sizeof *L);
    memset(L->key, DUI_NO_TILE, sizeof L->key);
    L->deck = *d;
    L->full = true;
    L->pages = 1;
    L->menu_col = L->menu_row = L->back_col = L->back_row = L->pager_col = L->pager_row = -1;
    L->status_at = d->bar == DUI_BAR_STRIP ? DUI_STATUS_STRIP : d->bar == DUI_BAR_INFO ? DUI_STATUS_INFO : DUI_STATUS_KEYS;
}

dui_tile_t *dui__lay_put(dui_layout_t *L, int page, int col, int row, int kind)
{
    static dui_tile_t sink;
    const int cols = L->deck.cols;
    if (L->ntiles >= DUI_LAYOUT_TILES || col < 0 || row < 0 || col >= cols || row >= L->deck.rows || page < 0 ||
        page >= DUI_PAGES_MAX) {
        memset(&sink, 0, sizeof sink);
        L->overflow = true;
        return &sink;
    }
    dui_tile_t *t = &L->tile[L->ntiles];
    memset(t, 0, sizeof *t);
    t->kind = (uint8_t)kind;
    L->key[page][row * cols + col] = L->ntiles++;
    return t;
}

void dui__lay_align_row0(dui_layout_t *L)
{
    dui_tile_t *row[DUI_KEYS_MAX];
    int n = 0;
    for (int c = 0; c < L->deck.cols && c < DUI_KEYS_MAX; c++) {
        const uint8_t i = L->key[0][c];
        if (i != DUI_NO_TILE && i < L->ntiles && L->tile[i].kind == DUI_TILE_WORD) row[n++] = &L->tile[i];
    }
    if (n) dui_align_status(row, n, L->deck.key_px); /* the status size, as on every status row */
}

dui_tile_t *dui__pad_key(dui_layout_t *L, int page, int col, int row, uint16_t action, const dui__pad_ids_t *ids)
{
    dui_tile_t *t = dui__lay_put(L, page, col, row, DUI_TILE_KEY);
    t->tint = DUI_TINT_MAIN;
    t->action = action;
    if (!action) return t;
    if (action >= ids->digit0 && action <= ids->digit0 + 9) t->text = dui__digit[action - ids->digit0];
    else if (action == ids->del) t->icon = DUI_ICON_DEL;
    else if (action == ids->cancel) t->icon = DUI_ICON_CLOSE;
    else if (action == ids->ok) t->text = ids->ok_text, t->flags |= DUI_F_SELECTED;
    else if (action == ids->down) t->text = "-";
    else if (action == ids->up) t->text = "+";
    else if (ids->add && action == ids->add) t->text = ids->add_text ? ids->add_text : "Add";
    return t;
}
