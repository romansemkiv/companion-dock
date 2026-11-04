/* ESP: the maintenance screens on the decks (on demand too),
 * on the surfaces task.
 *
 * One per deck with a screen: deckui builds the page's screen from a snapshot of the box (maintmap.c),
 * plans it for the model, and its navigation turns the deck's input and time into pages, actions and
 * sounds. Each pass every key's tile is fingerprinted with its press state, hold fill, hint and animation
 * phase (5 fps), and only the keys whose fingerprint changed are drawn and queued as box-drawn frames
 * through imgwork; the strip and the info bar likewise.
 *
 * On demand, the box menu, opens and closes on each deck on its own (surf.od, the set of device_ids it
 * is open on); every other deck stays as it is. Over a deck Companion drives, the hand-over is tied to what the
 * deck shows:
 * - opening: in dock mode the chosen deck's input is held from Companion at once (surfaces_maint ->
 *   maintui_maint -> elgdock_hold_input with its serial: assigned to a dock again meanwhile, it starts
 *   held); in Satellite mode on_input stops sending its presses to Companion as soon as the flag is set,
 *   and the keys Companion saw pressed are released here; the deck unplugged closes it;
 * - closing: the deck's input goes to nobody (surf.handback) until Companion's images are on it: handed
 *   to usbdeck (Satellite: the cache's frames queued to imgwork are taken and handed on; dock mode:
 *   dockmode's cache written back) and then written by it (usbdeck_writes_drained), or SURF_HANDBACK_US
 *   (3 s) at most; then its dock hold ends, with a key held through the session passed on as
 *   released until it is let go (ingate.h). The hand-back is per deck: a reopen on another deck never
 *   cancels it (dock_hold, surf_held_decks). */
#include <stdio.h>
#include <string.h>

#include "deckui/deckui.h"
#include "deckui/nav.h"
#include "deckui/ipedit.h"
#include "deckui/pin.h"
#include "deckui/screens.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "feedback/feedback.h"
#include "maintmap.h"
#include "netmgr/netmgr.h"
#include "satclient/satclient.h"
#include "slotsync/slotsync.h"
#include "surfaces_int.h"
#include "surfaces_priv.h"

#define TAG "maintui"

#define SNAP_US (2000 * 1000LL)      /* the snapshot is made again this often (RSSI, uptime, sync progress) */
#define PENDING_US (30 * 1000000LL)  /* "pending verify" read again this often (it reads flash) */
#define STACK_LOG_US (5000 * 1000LL) /* the stack marks are logged this long after the first full screen */
#define UI_COMPS 8

/* A maintenance screen's frame: queued while the slot shows the UI (also on a deck Companion drives, on
 * demand: imgwork hands it to dockmode in dock mode), never over a pending Companion image. Returns true
 * when queued. */
static bool put_ui(int slot, int idx, job_t *j)
{
    void *drop = j;
    surf_lock();
    if (surf.gen[slot] == j->gen && surf.ui[slot]) {
        job_t *p = surf.co.pending[slot][idx];
        if (!p || p->local) drop = coalesce_put(&surf.co, slot, idx, j);
    }
    surf_unlock();
    if (drop) surf_job_free(drop);
    return drop != j;
}


/* ---- the maintenance screens ----
 * One per deck with a screen, on the surfaces task: deckui builds the page's screen from a snapshot of
 * the box (maintmap.c), plans it for the model, and its navigation turns the deck's input and time into
 * pages, actions and sounds. Each pass every key's tile is fingerprinted with its press state, hold
 * fill, hint and animation phase (5 fps), and only the keys whose fingerprint changed are drawn and
 * queued (as box-drawn frames, through imgwork); the strip and the info bar likewise. */

typedef struct {
    bool shown;
    bool od;               /* the navigation knows the screen is on demand */
    uint32_t gen;          /* the slot's generation it was drawn for */
    const usbdeck_t *deck; /* compared only */
    const deck_model_t *m;
    dui_deck_t dd;
    int nctl;
    deck_ctl_t ctls[SURF_MAX_CTLS];
    int8_t key_ctl[DUI_KEYS_MAX]; /* the LCD key's control at row * dd.cols + col (ctl_at), -1 none (ui_start) */
    dui_screen_t scr;
    dui_layout_t L;
    dui_nav_t nav;
    dui_box_t box;         /* the snapshot the screen was built from */
    uint32_t fp[SURF_MAX_CTLS]; /* per control: the fingerprint of the frame queued (0: none) */
    int bright;            /* the brightness the box set on the deck (-1: none) */
    dui_pin_t pin;         /* the PIN pad over the screen */
    dui_layout_t PL;       /* ... its keys */
    uint32_t pin_ticket;   /* the check asked for last: an answer for another is stale */
    dui_unlock_t unlock;   /* this deck's locked actions unlocked with the PIN: its own, per deck */
    dui_ipe_t ipe;         /* the static-address editor over the screen (its layout in PL too) */
    char toast[40];        /* a notice for this deck only ("Unlocked · hold Restart"), over the box-wide one */
    uint8_t toast_state;   /* ... its outcome (DUI_STATE_*) */
    int64_t toast_until_us; /* ... shown until then (0: none) */
} ui_t;


static ui_t *s_ui[SURF_MAX_DECKS]; /* PSRAM, made at start */
static char s_dock_held[SURF_MAX_DECKS + 1][33]; /* dock mode: the serials elgdock_hold_input holds (s_hold_mtx) */
static int s_dock_nheld;
static SemaphoreHandle_t s_hold_mtx; /* dock_hold: any task on opening, the surfaces task on closing */
static bool s_refused[DUI_A_COUNT]; /* a refused action logged once */
static uint8_t *s_bar;             /* the strip, drawn whole before it is cut into its cells (PSRAM) */
static int64_t s_snap_at_us, s_pending_at_us = -PENDING_US, s_full_at_us;
static bool s_pending, s_stack_logged;

static int64_t now_ms_of(int64_t us) { return us / 1000; }

bool maintui_redraw(int slot)
{
    ui_t *u = s_ui[slot];
    if (!u || !u->shown) return false;
    memset(u->fp, 0, sizeof u->fp);
    return true;
}

/* The running image is pending verify (otadata: read from flash, at most every PENDING_US). */
static bool pending_verify(int64_t now)
{
    if (now - s_pending_at_us >= PENDING_US) {
        esp_ota_img_states_t st;
        const esp_partition_t *run = esp_ota_get_running_partition();
        s_pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
        s_pending_at_us = now;
    }
    return s_pending;
}

/* What every deck's snapshot shares, gathered once a pass when it is due. */
typedef struct {
    netmgr_status_t ns;
    slotsync_status_t ss;
    surf_settings_t set;
    companion_instance_t inst[UI_COMPS];
    char host[64], ap_ssid[33], ap_pass[65], version[32], toast[40];
    uint8_t toast_state;
    dui_ipset_t eth_saved, eth_lease; /* the saved static set, Ethernet's address now */
    surf_maint_in_t in;
} snap_t;

/* the surfaces task's own statics, in PSRAM (maintui_init; internal when PSRAM is short): off its stack, touched by
 * that task only, never by DMA, an ISR or with the cache off (its flash reads, pending_verify, take none of them) */
typedef struct {
    snap_t snap;                          /* the shared part of every deck's snapshot */
    char comp_id[DUI_COMPS_MAX][64];      /* the Companions the Choose home and page offer, by pick index */
    char gone[SURF_MAX_DECKS][48];        /* maintui_update: the decks the box menu closed on, unplugged */
    surf_slot_t sl;                       /* maintui_update: a copy of a slot (~1 KB) */
} ui_statics_t;
static ui_statics_t *s_st; /* NULL: no memory: no maintenance screen (maintui_update does nothing) */
#define s_snap (s_st->snap)
#define s_comp_id (s_st->comp_id)

static void gather(snap_t *g, int64_t now)
{
    surf_lock(); /* the settings as given (surfaces_set_settings): never the config lock here */
    g->set = surf.settings;
    surf_unlock();
    if (!g->set.brightness) g->set.brightness = 70; /* none given yet: deckBrightness's default */
    netmgr_get_status(&g->ns);
    slotsync_get(&g->ss);
    int ninst = 0;
    sat_caps_t caps = {0};
    satc_state_t st = SATC_IDLE;
    g->host[0] = '\0';
    if (!surf.dock) { /* the Satellite client runs in Satellite mode only */
        ninst = discovery_instances(g->inst, UI_COMPS);
        satclient_get_target(g->host, sizeof g->host, NULL);
        satclient_caps(&caps);
        st = satclient_state();
    }
    char too_old_host[64];
    surf_lock();
    const sel_result_t sel = surf.sel;
    const bool ap_shown = surf.ap_shown, ap_open = surf.ap_open, live = surf.live, too_old_flag = surf.too_old;
    snprintf(g->ap_ssid, sizeof g->ap_ssid, "%s", surf.ap_ssid);
    snprintf(g->ap_pass, sizeof g->ap_pass, "%s", surf.ap_pass);
    snprintf(too_old_host, sizeof too_old_host, "%s", surf.too_old_host);
    snprintf(g->toast, sizeof g->toast, "%s", now < surf.toast_until_us ? surf.toast : "");
    g->toast_state = surf.toast_state;
    surf_unlock();
    for (int i = 0; i < DUI_COMPS_MAX; i++) /* the pick index of each Companion offered */
        snprintf(s_comp_id[i], sizeof s_comp_id[i], "%s", i < ninst ? g->inst[i].id : "");
    snprintf(g->version, sizeof g->version, "%s", esp_app_get_description()->version);
    dui_ipset_t *sv = &g->eth_saved, *ls = &g->eth_lease;
    snprintf(sv->ip, sizeof sv->ip, "%s", g->set.eth_ip);
    snprintf(sv->mask, sizeof sv->mask, "%s", g->set.eth_mask);
    snprintf(sv->gw, sizeof sv->gw, "%s", g->set.eth_gw);
    snprintf(sv->dns, sizeof sv->dns, "%s", g->set.eth_dns);
    netmgr_eth_lease(ls->ip, ls->mask, ls->gw, ls->dns);
    g->in = (surf_maint_in_t){
        .dock = surf.dock, .ns = &g->ns, .ss = &g->ss, .box_name = g->set.box_name, .wifi_main = g->set.wifi_main,
        .wifi_backup = g->set.wifi_backup, .ap_shown = ap_shown, .ap_open = ap_open, .ap_ssid = g->ap_ssid,
        .ap_pass = g->ap_pass, .brightness = g->set.brightness, .sound = g->set.sound, .locked = g->set.locked,
        .ondemand = atomic_load(&surf.ondemand), .version = g->version, .uptime_s = (uint32_t)(now / 1000000),
        .pending = pending_verify(now), .sel = sel, .companion_auto = g->set.companion_auto, .flash = g->set.led_flash,
        .companion_host = g->set.companion_host, .companion_port = g->set.companion_port, .deck_limit = (uint8_t)surf.limit,
        .too_old = st == SATC_TOO_OLD || (too_old_flag && !strcmp(too_old_host, g->host)), .live = live,
        .target_host = g->host, .companion_version = caps.companion_version, .comps = g->inst, .ncomps = ninst,
        .toast = g->toast, .toast_state = g->toast_state, .eth_dhcp = g->set.eth_dhcp, .eth_saved = &g->eth_saved, .eth_lease = &g->eth_lease,
    };
}

/* The slot's snapshot (the shared part gathered): its own deck's part laid over it. */
static void snapshot(int slot, const snap_t *g, dui_box_t *b)
{
    surf_maint_in_t in = g->in;
    char peer[16] = "";
    surf_lock();
    const surf_slot_t *sl = &surf.reg.slot[slot];
    const usbdeck_t *d = sl->deck;
    in.connected = surf.dock ? surf.owned[slot] : surf.live && sl->added;
    in.ondemand = surf_od_slot_locked(slot); /* the Close key on the decks it is open on */
    const ui_t *u = s_ui[slot];
    in.locked = dui_deck_locked(g->in.locked, u && u->unlock.on); /* unlocked on this deck only */
    if (u && u->toast_until_us) { /* this deck's own notice */
        in.toast = u->toast;
        in.toast_state = u->toast_state;
    }
    if (surf.dock && surf.owned[slot]) snprintf(peer, sizeof peer, "%s", surf.peer[slot]);
    surf_unlock();
    in.dock_port = surf.dock ? dockmode_port(d) : 0; /* d only compared */
    in.dock_peer = peer;
    surf_maint_box(b, &in);
}

static void rebuild(ui_t *u)
{
    dui_build(&u->scr, dui_nav_page(&u->nav), &u->box);
    dui_plan(&u->dd, &u->scr, &u->L);
    dui_nav_layout(&u->nav, &u->L);
}

static void close_ondemand(int slot, const char *why);
static void pin_open(int slot, uint16_t from_action);
static void pin_sound(uint8_t sound);
static void ipe_open(int slot);
static void ipe_open_host(int slot);

/* What the navigation asked for. idle_close: it came from the time passing (60 s without a press). */
static void apply_nav(int slot, const dui_nav_out_t *o, bool idle_close)
{
    ui_t *u = s_ui[slot];
    if (o->rebuild) rebuild(u);
    pin_sound(o->sound); /* the same switch */
    if (o->action) {
        const int a = o->action;
        const bool pick = a >= DUI_A_PICK0 && a <= DUI_A_PICK5;
        const char *arg = pick ? s_comp_id[a - DUI_A_PICK0] : "";
        if (!dui_action_allowed(a, s_snap.in.locked, u->unlock.on)) { /* logged once per action */
            if (a < DUI_A_COUNT && !s_refused[a])
                ESP_LOGW(TAG, "deck action %d refused: a settings PIN is set", a);
            if (a < DUI_A_COUNT) s_refused[a] = true;
        } else if (pick && !arg[0]) {
            ESP_LOGW(TAG, "deck pick %d: that Companion is no longer listed", a - DUI_A_PICK0);
        } else if (a == DUI_A_ETH_EDIT) { /* the editor, here: nothing for the worker until Save */
            ipe_open(slot);
        } else if (a == DUI_A_COMP_EDIT) { /* the editor in host mode, likewise */
            ipe_open_host(slot);
        } else {
            ESP_LOGI(TAG, "slot %d: deck action %d%s%s%s", slot, a, arg[0] ? " " : "", arg,
                     dui_action_locked(a) && u->unlock.on ? " (unlocked with the PIN)" : "");
            if (surf.hooks.action) surf.hooks.action(a, arg, u->unlock.on);
        }
    }
    if (o->close) close_ondemand(slot, idle_close ? "60 s without a press" : "its Close key"); /* this deck only */
    if (o->pin) pin_open(slot, o->pin_action);
}

/* The deck's control at (col, row) of the given kind; -1 none. */
static int ctl_at(const ui_t *u, deck_ctl_kind_t kind, int col, int row)
{
    for (int i = 0; i < u->nctl; i++)
        if (u->ctls[i].kind == kind && u->ctls[i].col == col && u->ctls[i].row == row) return i;
    return -1;
}

/* the LCD key's control at (col, row), from ui_start's table (what ctl_at finds); -1 none. */
static int key_ctl_at(const ui_t *u, int col, int row)
{
    const int i = row * u->dd.cols + col;
    return i < DUI_KEYS_MAX ? u->key_ctl[i] : ctl_at(u, CTL_KEY_LCD, col, row);
}

/* The editor's layout, with the deck's lock and the status indicators of the screen under it. */
static void ipe_layout(ui_t *u, int64_t ms)
{
    u->ipe.locked = u->box.locked;
    u->ipe.net = u->scr.status.net;
    u->ipe.rssi = u->scr.status.rssi;
    u->ipe.hotspot = u->scr.status.hotspot;
    u->ipe.wifi = u->scr.status.wifi; /* the signs */
    u->ipe.eth_link = u->scr.status.eth_link;
    dui_ipe_layout(&u->dd, &u->ipe, ms, &u->PL);
}

/* Queues the keys, strip cells, info bar and touch-key lights whose fingerprint changed. Returns the
 * number still not queued (a Companion image pending, or no memory: tried again next pass). */
static int ui_render(int slot, int64_t now)
{
    ui_t *u = s_ui[slot];
    const int64_t ms = now_ms_of(now);
    const bool pad = u->pin.open; /* the PIN pad over the screen */
    const bool ed = !pad && u->ipe.open; /* the static-address editor over it */
    if (pad) dui_pin_layout(&u->dd, &u->pin, ms, &u->PL);
    if (ed) ipe_layout(u, ms);
    const dui_layout_t *Lr = pad || ed ? &u->PL : &u->L;
    const int pg = pad ? u->pin.pg : ed ? u->ipe.pg : dui_nav_pg(&u->nav), px = u->dd.key_px;
    int left = 0;
    for (int r = 0; r < u->dd.rows; r++)
        for (int c = 0; c < u->dd.cols; c++) {
            const int idx = key_ctl_at(u, c, r);
            if (idx < 0) continue;
            dui_tile_t t;
            dui_layout_key(Lr, pg, c, r, &t);
            if (pad) dui_pin_decorate(&u->pin, c, r, &t);
            else if (ed) dui_ipe_decorate(&u->ipe, c, r, ms, &t);
            else dui_nav_decorate(&u->nav, c, r, ms, &t);
            if (dui_tile_animated(&t)) t.phase = dui_phase(ms, t.kind == DUI_TILE_SPIN ? DUI_SPIN_MS : DUI_PULSE_MS);
            const uint32_t h = dui_tile_hash(&t);
            if (h == u->fp[idx]) continue;
            job_t *j = surf_job_new(JOB_RGB, (size_t)px * px * 3, u->gen, true);
            if (!j) {
                left++;
                continue;
            }
            dui_render_key(&t, px, j->data);
            if (put_ui(slot, idx, j))
                u->fp[idx] = h;
            else
                left++;
        }
    if (u->dd.bar != DUI_BAR_NONE) {
        dui_status_t st = u->scr.status;
        /* the whole bar is drawn and encoded each time its glyph moves: 2 fps (the keys' glyph: 5) */
        if (!u->scr.sub && st.state == DUI_STATE_WAIT) st.phase = dui_bar_phase(ms);
        const uint32_t h = pad  ? dui_pin_bar_hash(&u->pin, ms)
                           : ed ? dui_ipe_bar_hash(&u->ipe, ms)
                                : dui_bar_hash(&st, u->scr.sub, pg, u->L.pages);
        const int w = u->dd.bar_w, bh = u->dd.bar_h;
        bool due = false, drawn = false;
        for (int i = 0; i < u->nctl; i++)
            due |= (u->ctls[i].kind == CTL_LCD_CELL || u->ctls[i].kind == CTL_INFO) && u->fp[i] != h;
        for (int i = 0; due && i < u->nctl; i++) {
            const deck_ctl_t *c = &u->ctls[i];
            if ((c->kind != CTL_LCD_CELL && c->kind != CTL_INFO) || u->fp[i] == h) continue;
            job_t *j = surf_job_new(JOB_RGB, (size_t)c->w * c->h * 3, u->gen, true);
            if (!j) {
                left++;
                continue;
            }
            if (c->kind == CTL_INFO) {
                if (pad) dui_render_pin_bar(&u->pin, ms, c->w, c->h, j->data);
                else if (ed) dui_render_ipe_bar(&u->ipe, ms, c->w, c->h, j->data);
                else dui_render_bar(&st, u->scr.sub, pg, u->L.pages, c->w, c->h, j->data);
            } else { /* the strip drawn whole once, then cut: each cell its region */
                if (!drawn && s_bar && w <= 1200 && bh <= 100) {
                    if (pad) dui_render_pin_bar(&u->pin, ms, w, bh, s_bar);
                    else if (ed) dui_render_ipe_bar(&u->ipe, ms, w, bh, s_bar);
                    else dui_render_bar(&st, u->scr.sub, pg, u->L.pages, w, bh, s_bar);
                }
                drawn = true;
                uint16_t x, y, cw, ch;
                deck_lcd_cell_region(u->m, c, &x, &y, &cw, &ch);
                for (int row = 0; row < c->h; row++) {
                    if (s_bar && w <= 1200 && row < bh && bh <= 100 && x + c->w <= w)
                        memcpy(j->data + (size_t)row * c->w * 3, s_bar + ((size_t)row * w + x) * 3, (size_t)c->w * 3);
                    else
                        memset(j->data + (size_t)row * c->w * 3, 0, (size_t)c->w * 3);
                }
            }
            if (put_ui(slot, i, j))
                u->fp[i] = h;
            else
                left++;
        }
    }
    bool dock_companion; /* dock mode, on demand over Companion: its driver keeps the colour keys */
    surf_lock();
    dock_companion = surf.dock && surf.ui_od[slot];
    surf_unlock();
    for (int i = 0; !dock_companion && i < u->nctl; i++) { /* the Neo's touch keys: lit while there are pages */
        if (u->ctls[i].kind != CTL_KEY_RGB) continue;
        const bool lit = Lr->pages > 1;
        const uint32_t h = lit ? 2 : 1;
        if (u->fp[i] == h) continue;
        job_t *j = surf_job_new(JOB_SOLID, 0, u->gen, true);
        if (!j) continue;
        j->r = lit ? 0x5C : 0x10; /* SUI_RGB_ACCENT, else a glow */
        j->g = lit ? 0xD6 : 0x10;
        j->b = lit ? 0xF0 : 0x10;
        if (put_ui(slot, i, j)) u->fp[i] = h;
    }
    surf_kick_imgwork();
    return left;
}

/* the deck's brightness while it shows the maintenance screen, the box's own or on demand over
 * Companion: deckBrightness, at once on every change. Over Companion in dock mode it goes through dockmode
 * (between Companion's images; Companion's last one comes back when on demand ends); else to the deck. */
static void ui_brightness(int slot, uint8_t pct)
{
    ui_t *u = s_ui[slot];
    const int want = surf_bright_apply(pct, u->bright);
    if (want < 0) return;
    bool owned = false;
    usbdeck_t *d = surf_deck_acquire(USER_SURF, slot, u->gen, &owned);
    esp_err_t e = ESP_ERR_INVALID_STATE;
    /* on owned as acquired (Companion may have just taken the deck: its dock task writes to it) */
    if (d && surf_bright_box_path(surf.dock, owned) == SURF_BRIGHT_DOCKCACHE) {
        dockmode_box_brightness(d, (uint8_t)want);
        e = ESP_OK;
    } else if (d) {
        e = usbdeck_set_brightness(d, (uint8_t)want);
    }
    surf_deck_release(USER_SURF);
    if (e == ESP_OK) u->bright = want;
}

/* on demand closed over Companion's deck (Satellite): Companion's last brightness back. In dock mode
 * dockmode does it when its hold ends. */
static void bright_handback(int slot)
{
    ui_t *u = s_ui[slot];
    surf_lock();
    const int last = surf.comp_bright[slot];
    surf_unlock();
    int pct = surf.dock ? -1 : surf_bright_handback(last, u->bright);
    u->bright = -1;
    if (pct < 0) return;
    usbdeck_t *d = surf_deck_acquire(USER_SURF, slot, u->gen, NULL);
    for (int i = 0; d && i < 3; i++) { /* one of Companion's that landed meanwhile is not undone */
        usbdeck_set_brightness(d, (uint8_t)pct);
        surf_lock();
        const int now_last = surf.comp_bright[slot];
        surf_unlock();
        if (now_last < 0 || now_last == pct) break;
        pct = now_last;
    }
    surf_deck_release(USER_SURF);
}

/* The slot starts showing the maintenance screen: its home, untouched. */
static void ui_start(int slot, const surf_slot_t *sl, uint32_t gen, int64_t now)
{
    ui_t *u = s_ui[slot];
    u->shown = true;
    u->gen = gen;
    u->deck = sl->deck;
    u->m = sl->model;
    u->nctl = sl->nctl;
    memcpy(u->ctls, sl->ctls, sizeof u->ctls);
    dui_deck_of(u->m, &u->dd);
    memset(u->key_ctl, -1, sizeof u->key_ctl); /* once here, not a scan of the controls per key per pass */
    for (int r = 0; r < u->dd.rows; r++)
        for (int c = 0; c < u->dd.cols; c++)
            if (r * u->dd.cols + c < DUI_KEYS_MAX) u->key_ctl[r * u->dd.cols + c] = (int8_t)ctl_at(u, CTL_KEY_LCD, c, r);
    if (u->dd.bar == DUI_BAR_STRIP && !s_bar) /* the widest strip: the + XL's 1200 x 100 */
        s_bar = heap_caps_malloc((size_t)1200 * 100 * 3, MALLOC_CAP_SPIRAM);
    memset(u->fp, 0, sizeof u->fp);
    u->bright = -1;
    u->od = false;
    dui_nav_init(&u->nav, &u->dd, now_ms_of(now));
    snapshot(slot, &s_snap, &u->box);
    rebuild(u);
    surf_lock();
    surf.ui[slot] = surf.gen[slot] == gen;
    surf_unlock();
    ESP_LOGI(TAG, "slot %d: maintenance screen on the %s", slot, u->m->name);
}

/* Companion's last images come back on the slot's deck from the cache, at pct (black where nothing is
 * kept; "Reconnecting" over the LCD keys with overlay), and its colour keys' colours (black where it gave
 * none, or cleared them: the box lit the Neo's touch keys meanwhile) (Satellite; in dock mode dockmode
 * writes its cache back by itself, so dock is false only). */
static void restore_last(int slot, uint8_t pct, bool overlay)
{
    uint8_t kinds[SURF_MAX_CTLS], rgb[SURF_MAX_CTLS][3];
    bool rgb_set[SURF_MAX_CTLS];
    int n = 0;
    surf_lock();
    const uint32_t gen = surf.gen[slot];
    if (!surf.dock && surf.reg.slot[slot].model) {
        n = surf.reg.slot[slot].nctl;
        for (int c = 0; c < n; c++) {
            kinds[c] = (uint8_t)surf.reg.slot[slot].ctls[c].kind;
            rgb_set[c] = surf.rgb_set[slot][c];
            memcpy(rgb[c], surf.rgb[slot][c], 3);
        }
    }
    surf_unlock();
    for (int c = 0; c < n; c++) {
        job_t *j = NULL;
        if (surf_is_image_ctl((deck_ctl_kind_t)kinds[c])) {
            j = surf_last_job(gen, pct, overlay && kinds[c] == CTL_KEY_LCD);
        } else if (kinds[c] == CTL_KEY_RGB && (j = surf_job_new(JOB_SOLID, 0, gen, true))) {
            j->r = rgb_set[c] ? rgb[c][0] : 0;
            j->g = rgb_set[c] ? rgb[c][1] : 0;
            j->b = rgb_set[c] ? rgb[c][2] : 0;
        }
        if (j) surf_put_local(slot, c, j, false); /* a pending Companion image stays: it is newer */
    }
    if (n) surf_kick_imgwork();
}

/* The slot stops showing the maintenance screen. Over Companion (on demand): its images come back and
 * the deck's input goes to nobody until they are (handback). grace_keep (Satellite, Companion lost a
 * moment ago with images kept): those images come back, dimmed with "Reconnecting" once the dim phase
 * began, as the other decks show them. */
static void ui_stop(int slot, bool grace_keep, int64_t now)
{
    ui_t *u = s_ui[slot];
    surf_lock();
    const bool od = surf.ui_od[slot];
    const bool dim = surf.dock ? surf.ddimmed[slot] : surf.dimmed; /* dock mode: this deck's own dim phase */
    surf.ui[slot] = surf.ui_od[slot] = false;
    if (od) { /* until Companion's images are back: nobody's input */
        surf.handback[slot] = true;
        surf.handback_until[slot] = now + SURF_HANDBACK_US;
    }
    surf_unlock();
    u->shown = false;
    dui_nav_cancel(&u->nav);
    dui_pin_close(&u->pin); /* the pad, and the unlock, end with the screen */
    dui_ipe_close(&u->ipe); /* the editor too, nothing saved */
    u->toast_until_us = 0;
    dui_unlock_step(&u->unlock, now_ms_of(now), true, false);
    surf.msg_seq_on[slot] = 0; /* a message the screen kept off comes back */
    if (od) {
        restore_last(slot, 100, false);
        bright_handback(slot);
    } else if (grace_keep)
        restore_last(slot, dim ? SURF_DIM_PCT : 100, dim);
    ESP_LOGI(TAG, "slot %d: maintenance screen gone%s", slot, od ? ": Companion's images coming back" : "");
}

/* Satellite, the screen on demand opening over a deck Companion drives: Companion lets go of what it saw
 * pressed. */
static void sat_release_all(int slot)
{
    char id[48];
    surf_lock();
    memcpy(id, surf.reg.slot[slot].device_id, sizeof id);
    surf_unlock();
    xSemaphoreTake(surf.inmtx, portMAX_DELAY);
    for (int i = 0; i < surf.nsat_down[slot]; i++) satclient_key(id, surf.sat_down[slot][i], false);
    surf.nsat_down[slot] = 0;
    xSemaphoreGive(surf.inmtx);
}

/* the box menu closed on the slot's deck alone; every other deck keeps its own. */
static void close_ondemand(int slot, const char *why)
{
    char id[48];
    surf_lock();
    memcpy(id, surf.reg.slot[slot].device_id, sizeof id);
    const bool was = surf_od_del(&surf.od, id);
    atomic_store(&surf.ondemand, surf.od.n > 0);
    surf_unlock();
    if (was) ESP_LOGI(TAG, "box menu closed on %s (%s)", id, why);
    atomic_store(&surf.snap_dirty, true);
}

/* ---- the PIN pad and the unlock ---- */

/* The deck's locked flag changed (unlocked, or the unlock ended): the screen again from a new snapshot. */
static void lock_changed(int slot)
{
    ui_t *u = s_ui[slot];
    dui_box_t b;
    snapshot(slot, &s_snap, &b);
    u->box = b;
    dui_nav_out_t o;
    dui_nav_box_changed(&u->nav, &o);
    apply_nav(slot, &o, false);
}

static void pin_sound(uint8_t sound)
{
    switch (sound) {
    case DUI_SOUND_CLICK: feedback_event(FB_EV_KEY_CLICK); break;
    case DUI_SOUND_CONFIRM: feedback_event(FB_EV_CONFIRM); break;
    case DUI_SOUND_LOCKED: feedback_event(FB_EV_LOCKED); break;
    default: break;
    }
}

/* What the pad asked for. */
static void apply_pin(int slot, const dui_pin_out_t *o, int64_t now)
{
    ui_t *u = s_ui[slot];
    const int64_t ms = now_ms_of(now);
    pin_sound(o->sound);
    if (o->check) { /* off this task: PBKDF2 runs on the actions' worker; the PIN never logged */
        char pin[DUI_PIN_MAX + 1];
        dui_pin_take(&u->pin, pin);
        const uint32_t ticket = ++u->pin_ticket;
        ESP_LOGI(TAG, "slot %d: PIN entered, checking", slot);
        dui_pin_out_t r = {0};
        if (!surf.hooks.pin_check) {
            dui_pin_result(&u->pin, DUI_PIN_R_NONE, 0, ms, &r);
        } else if (!surf.hooks.pin_check(slot, ticket, pin)) {
            /* busy (another deck's check, or the worker's queue full): answered here, never posted
             * back into this task's own queue; the digits stay for another try */
            ESP_LOGI(TAG, "slot %d: PIN check busy, try again", slot);
            dui_pin_busy(&u->pin, pin, ms, &r);
        }
        pin_sound(r.sound);
        for (size_t i = 0; i < sizeof pin; i++) ((volatile char *)pin)[i] = 0; /* a wipe kept, never elided */
    }
    if (o->closed && o->unlocked) {
        dui_unlock_start(&u->unlock, ms);
        ESP_LOGI(TAG, "slot %d: unlocked with the PIN (5 min at most, until it closes or idles)", slot);
        /* this deck's notice (the others are still locked), naming the key now unlocked and how it goes: "Unlocked ·
         * hold Restart"; that key pulses cyan for 3 s on the screen it came from */
        if (u->ipe.open) { /* the editor's Save: its own layout (the pad drew over u->PL) */
            ipe_layout(u, ms);
            dui_unlock_note(&u->PL, u->pin.from_action, u->toast, sizeof u->toast);
        } else {
            dui_unlock_note(&u->L, u->pin.from_action, u->toast, sizeof u->toast);
        }
        dui_nav_pulse(&u->nav, u->pin.from_action, ms);
        u->toast_state = DUI_STATE_OK;
        u->toast_until_us = now + TOAST_US; /* as the box-wide notice */
        lock_changed(slot); /* the key pressed is unlocked now, and did not fire */
    } else if (o->closed) {
        ESP_LOGI(TAG, "slot %d: PIN pad closed", slot);
    }
    /* the idle clock on the screen it came from runs from the pad's last press: its own 30 s idle close
     * is not a press (the 60 s) */
    if (o->closed) dui_nav_activity(&u->nav, o->last_press);
    if (u->shown) ui_render(slot, now);
}

static void pin_open(int slot, uint16_t from_action)
{
    ui_t *u = s_ui[slot];
    const int64_t now = esp_timer_get_time(), ms = now_ms_of(now);
    surf_lock();
    const bool have_pin = surf.settings.pin;
    surf_unlock();
    const int wait = have_pin && surf.hooks.pin_wait_s ? surf.hooks.pin_wait_s() : 0;
    dui_pin_out_t o;
    dui_pin_open(&u->pin, &u->dd, from_action, have_pin, wait, ms, &o);
    ESP_LOGI(TAG, "slot %d: PIN pad for locked action %d%s", slot, from_action,
             !have_pin ? " (no PIN set: set one on the web page)" : wait ? " (locked out)" : "");
    apply_pin(slot, &o, now);
}

/* ---- the static-address editor ---- */

/* What the editor asked for. */
static void apply_ipe(int slot, const dui_ipe_out_t *o, int64_t now)
{
    ui_t *u = s_ui[slot];
    pin_sound(o->sound);
    if (o->save && u->ipe.kind == DUI_IPE_HOST) { /* Companion.SetConfig's path, on the worker likewise */
        dui_ipset_t v;
        dui_ipe_values(&u->ipe, &v);
        ESP_LOGI(TAG, "slot %d: Companion IP saved on the deck: %s", slot, v.ip);
        if (surf.hooks.action) surf.hooks.action(DUI_A_COMP_SAVE, v.ip, u->unlock.on);
    } else if (o->save) { /* through the worker: Eth.SetConfig's path, refused there unless this deck is unlocked */
        dui_ipset_t v;
        dui_ipe_values(&u->ipe, &v);
        char arg[64];
        snprintf(arg, sizeof arg, "%s %s %s %s", v.ip, v.mask, v.gw, v.dns[0] ? v.dns : "-");
        ESP_LOGI(TAG, "slot %d: static address saved on the deck: %s", slot, arg);
        if (surf.hooks.action) surf.hooks.action(DUI_A_ETH_SAVE, arg, u->unlock.on);
    } else if (o->closed) {
        ESP_LOGI(TAG, "slot %d: %s editor closed, nothing saved", slot,
                 u->ipe.kind == DUI_IPE_HOST ? "Companion IP" : "static-address");
    }
    if (o->closed) dui_nav_activity(&u->nav, o->last_press); /* its own idle close is not a press */
    if (u->shown) ui_render(slot, now);
}

static void ipe_open(int slot)
{
    ui_t *u = s_ui[slot];
    const int64_t now = esp_timer_get_time(), ms = now_ms_of(now);
    dui_ipset_t pre; /* the static set if valid, else the lease, else empty */
    const int from = dui_eth_static_pick(&s_snap.eth_saved, &s_snap.eth_lease, &pre);
    dui_ipe_out_t o;
    dui_ipe_open(&u->ipe, &u->dd, from == DUI_ETH_FROM_NONE ? NULL : &pre, ms, &o);
    ESP_LOGI(TAG, "slot %d: static-address editor opened (%s)", slot,
             from == DUI_ETH_FROM_SAVED ? "the saved set" : from == DUI_ETH_FROM_LEASE ? "the DHCP lease" : "empty");
    apply_ipe(slot, &o, now);
}

/* Set IP's editor (Satellite), prefilled with the Companion it connects to when that is an IPv4 address,
 * else the saved companionHost when it is one, else empty; the summary shows companionPort. */
static void ipe_open_host(int slot)
{
    ui_t *u = s_ui[slot];
    const int64_t now = esp_timer_get_time(), ms = now_ms_of(now);
    const char *ip = u->box.sat.target.ip[0] ? u->box.sat.target.ip : u->box.sat.saved_ip;
    dui_ipe_out_t o;
    dui_ipe_open_host(&u->ipe, &u->dd, ip, u->box.sat.port, ms, &o);
    ESP_LOGI(TAG, "slot %d: Companion IP editor opened (%s)", slot,
             u->box.sat.target.ip[0] ? "the Companion it connects to" : ip[0] ? "the saved address" : "empty");
    apply_ipe(slot, &o, now);
}

/* The pad closes and the unlock ends: the deck stopped showing the screen, or on demand closed on it. */
static void pin_session_over(int slot, int64_t now)
{
    ui_t *u = s_ui[slot];
    const bool was_open = u->pin.open || u->ipe.open;
    dui_pin_close(&u->pin);
    dui_ipe_close(&u->ipe); /* closing on demand ends the editor, nothing saved */
    const bool ended = dui_unlock_step(&u->unlock, now_ms_of(now), true, false);
    if (ended) ESP_LOGI(TAG, "slot %d: the PIN unlock ended (the maintenance screen closed)", slot);
    if ((ended || was_open) && u->shown) lock_changed(slot);
}

void maintui_pin_result(int slot, uint32_t ticket, int result, int wait_s)
{
    ui_t *u = slot >= 0 && slot < SURF_MAX_DECKS ? s_ui[slot] : NULL;
    if (!u || !u->shown || !u->pin.open || ticket != u->pin_ticket) return; /* cancelled, or another deck */
    const int64_t now = esp_timer_get_time();
    int r = DUI_PIN_R_NONE, w = wait_s;
    switch (result) {
    case SURF_PIN_OK: r = DUI_PIN_R_OK; break;
    case SURF_PIN_WRONG: r = DUI_PIN_R_WRONG; break;
    case SURF_PIN_LOCKED: r = DUI_PIN_R_LOCKED; break;
    case SURF_PIN_BUSY: r = DUI_PIN_R_BUSY; break; /* not sent any more, mapped for safety */
    default: break;
    }
    ESP_LOGI(TAG, "slot %d: PIN %s", slot,
             r == DUI_PIN_R_OK      ? "right"
             : r == DUI_PIN_R_WRONG  ? "wrong"
             : r == DUI_PIN_R_LOCKED ? "refused: locked out"
             : r == DUI_PIN_R_BUSY   ? "busy"
                                     : "not set");
    dui_pin_out_t o;
    dui_pin_result(&u->pin, r, w, now_ms_of(now), &o);
    apply_pin(slot, &o, now);
}

void maintui_input(int slot, uint32_t gen, const deck_event_t *ev)
{
    ui_t *u = slot >= 0 && slot < SURF_MAX_DECKS ? s_ui[slot] : NULL;
    if (!u || !u->shown || u->gen != gen) return; /* it changed since: dropped (a press never half-done) */
    const int64_t now = esp_timer_get_time(), ms = now_ms_of(now);
    const surf_nav_ev_t n = surf_nav_event(u->m, ev);
    if (u->pin.open) { /* the pad takes the deck's input while it is open */
        dui_pin_out_t po = {0};
        dui_pin_layout(&u->dd, &u->pin, ms, &u->PL);
        switch (n.kind) {
        case SURF_NAV_KEY: dui_pin_key(&u->pin, &u->PL, n.col, n.row, n.down, ms, &po); break;
        case SURF_NAV_DIAL: dui_pin_dial_turn(&u->pin, n.steps, ms, &po); break;
        case SURF_NAV_BACK: dui_pin_dial_press(&u->pin, ms, &po); break;
        case SURF_NAV_TOUCH: dui_pin_touch(&u->pin, n.steps, ms, &po); break;
        default: return;
        }
        dui_nav_activity(&u->nav, ms); /* the screen under it doesn't go idle meanwhile */
        apply_pin(slot, &po, now);
        return;
    }
    if (u->ipe.open) { /* the editor takes the deck's input while it is open */
        dui_ipe_out_t eo = {0};
        ipe_layout(u, ms);
        switch (n.kind) {
        case SURF_NAV_KEY: dui_ipe_key(&u->ipe, &u->PL, n.col, n.row, n.down, ms, &eo); break;
        case SURF_NAV_DIAL: dui_ipe_dial_turn(&u->ipe, n.steps, ms, &eo); break;
        case SURF_NAV_BACK: dui_ipe_dial_press(&u->ipe, ms, &eo); break;
        case SURF_NAV_TOUCH: dui_ipe_touch(&u->ipe, n.steps, ms, &eo); break;
        default: return;
        }
        apply_ipe(slot, &eo, now);
        return;
    }
    dui_nav_out_t o = {0};
    switch (n.kind) {
    case SURF_NAV_BAR: dui_nav_bar_tap(&u->nav, &u->scr.status, u->dd.bar_w, u->dd.bar_h, n.x, ms, &o); break;
    case SURF_NAV_KEY: dui_nav_key(&u->nav, &u->L, n.col, n.row, n.down, ms, &o); break;
    case SURF_NAV_DIAL: dui_nav_dial_turn(&u->nav, n.steps, ms, &o); break;
    case SURF_NAV_BACK: dui_nav_dial_press(&u->nav, ms, &o); break;
    case SURF_NAV_TOUCH: dui_nav_touch(&u->nav, n.steps, ms, &o); break;
    default: return;
    }
    apply_nav(slot, &o, false);
    if (u->shown) ui_render(slot, now);
}

/* Dock mode: the decks' input held from Companion (elgdock_hold_input, by serial): every deck the box
 * menu is open on, and every deck the screen still covers or that is still handing back (surf_held_decks): opening
 * or closing one never ends another's pending hand-back. s_hold_mtx makes the look and the change one step, so
 * closing and reopening at the same moment can never leave a deck off: an open adds to surf.od before it takes
 * the lock, and ui_stop turns ui_od into handback in one step under mtx. */
static void dock_hold(void)
{
    if (!surf.dock || !s_hold_mtx || !s_st) return; /* no state block (maintui_init found no memory) */
    xSemaphoreTake(s_hold_mtx, portMAX_DELAY);
    char want[SURF_MAX_DECKS + 1][33];
    int n;
    surf_lock(); /* s_hold_mtx before mtx */
    surf_hold_cand_t c[SURF_MAX_DECKS];
    for (int i = 0; i < SURF_MAX_DECKS; i++)
        c[i] = (surf_hold_cand_t){.device_id = surf.reg.slot[i].device_id, .covered = surf.ui_od[i],
                                  .handing_back = surf.handback[i]};
    const char *ids[SURF_MAX_DECKS + 1];
    n = surf_held_decks(&surf.od, c, SURF_MAX_DECKS, ids, SURF_MAX_DECKS + 1);
    for (int i = 0; i < n; i++) snprintf(want[i], sizeof want[i], "%.32s", surf_serial_of(ids[i])); /* the serial */
    surf_unlock();
    bool same = n == s_dock_nheld;
    for (int i = 0; same && i < n; i++) same = !strcmp(want[i], s_dock_held[i]);
    if (!same) {
        const char *list[SURF_MAX_DECKS + 1];
        for (int i = 0; i < n; i++) {
            memcpy(s_dock_held[i], want[i], sizeof s_dock_held[i]); /* both 33 bytes, terminated */
            list[i] = s_dock_held[i];
        }
        s_dock_nheld = n;
        dockmode_hold_input(list, n);
    }
    xSemaphoreGive(s_hold_mtx);
}

void maintui_maint(bool on)
{
    /* opening (the deck already in surf.od): held at once, before the deck is drawn (the surfaces
     * task may be busy for a while); closing waits for the hand-back (maintui_update), which then lets go */
    if (on) dock_hold();
}

/* The hand-back after the screen on demand left the deck it was on, Companion's: over once Companion's
 * images are on it (surf_handback_step: handed to usbdeck, then written by it), or SURF_HANDBACK_US at the
 * latest. True while it is still handing back. */
static bool handback_step(int64_t now)
{
    bool any = false;
    for (int slot = 0; slot < SURF_MAX_DECKS; slot++) {
        surf_lock();
        const bool hb = surf.handback[slot];
        if (!hb) { /* nothing to hand back: no look at its pending images */
            surf_unlock();
            continue;
        }
        const usbdeck_t *dk = surf.reg.slot[slot].deck;
        const uint32_t gen = surf.gen[slot];
        const int64_t until = surf.handback_until[slot];
        bool handed = !surf.img_at[slot]; /* Satellite: nothing pending, imgwork not at one of its frames */
        for (int c = 0; handed && c < SURF_MAX_CTLS; c++) handed = surf.co.pending[slot][c] == NULL;
        surf_unlock();
        if (surf.dock) handed = dockmode_settled(dk); /* dk only compared */
        /* then whether usbdeck has written it all: looked at after handed, so a frame handed on between
         * the two looks is still waited for */
        usbdeck_t *d = surf_deck_acquire(USER_SURF, slot, gen, NULL);
        const bool drained = usbdeck_writes_drained(d); /* NULL (gone meanwhile): true */
        surf_deck_release(USER_SURF);
        const surf_handback_t r = surf_handback_step(handed, drained, now, until);
        if (r == SURF_HB_WAIT) {
            any = true;
            continue;
        }
        surf_lock();
        if (surf.gen[slot] == gen) surf.handback[slot] = false;
        surf_unlock();
        ESP_LOGI(TAG, "slot %d: Companion's images %s: its input goes to Companion", slot,
                 r == SURF_HB_LATE ? "not all written yet, but the time is up" : "back on the deck");
    }
    return any;
}

bool maintui_update(int64_t now, bool msg)
{
    if (!s_st) return false; /* maintui_init found no memory for its statics */
    surf_lock();
    const bool live = surf.live;
    surf_unlock();
    /* the decks the box menu is open on. on_detach closes it on an unplugged deck (a replug comes back
     * closed); pruned here too, so a deck no longer attached with a screen never stays in the set. */
    bool od_on[SURF_MAX_DECKS] = {false};
    const usbdeck_t *od_decks[SURF_MAX_DECKS] = {NULL}; /* each deck at its slot's place, never packed: opening or
                                                         * closing another deck never moves it (dockmode reads them one
                                                         * at a time) */
    if (atomic_load(&surf.ondemand)) {
        char (*const gone)[48] = s_st->gone; /* in PSRAM */
        surf_od_cand_t c[SURF_MAX_DECKS];
        surf_lock();
        surf_od_cands_locked(c);
        const int ngone = surf_od_prune(&surf.od, c, SURF_MAX_DECKS, gone, SURF_MAX_DECKS);
        atomic_store(&surf.ondemand, surf.od.n > 0);
        for (int i = 0; i < SURF_MAX_DECKS; i++)
            if ((od_on[i] = surf_od_slot_locked(i))) od_decks[i] = surf.reg.slot[i].deck;
        surf_unlock();
        for (int i = 0; i < ngone && i < SURF_MAX_DECKS; i++) ESP_LOGI(TAG, "box menu closed on %s: its deck was unplugged", gone[i]);
        if (ngone) atomic_store(&surf.snap_dirty, true);
    }
    if (surf.dock) dockmode_maint(od_decks, SURF_MAX_DECKS); /* d only compared; NULL entries for the others */
    const bool grace = !live && surf.lost_at_us && now - surf.lost_at_us < STATUS_AFTER_US;
    surf_lock();
    const bool toast_over = surf.toast_until_us && now >= surf.toast_until_us;
    if (toast_over) surf.toast_until_us = 0;
    surf_unlock();
    const bool snap = atomic_exchange(&surf.snap_dirty, false) || toast_over || now - s_snap_at_us >= SNAP_US;
    if (snap) {
        gather(&s_snap, now);
        s_snap_at_us = now;
    }
    bool any = false, full = true;
    for (int slot = 0; slot < SURF_MAX_DECKS; slot++) {
        ui_t *u = s_ui[slot];
        if (!u) continue;
        surf_slot_t *const sl = &s_st->sl; /* a copy: ~1 KB, off the stack (in PSRAM) */
        bool any_last = false, companion = false, used, dloss = false;
        uint32_t gen;
        surf_lock();
        used = surf.reg.slot[slot].device_id[0] && surf.reg.slot[slot].model;
        if (used) *sl = surf.reg.slot[slot];
        gen = surf.gen[slot];
        if (used) companion = surf.dock ? surf.owned[slot] : live && sl->added;
        for (int k = 0; k < SURF_MAX_CTLS && !any_last; k++) any_last = surf.last[slot][k] != NULL;
        if (surf.dock) { /* this deck's own loss phase: Companion's images on it, dimmed from SURF_LOSS_DIM_US */
            const surf_loss_t ph = surf_loss_phase(surf.dlost_at_us[slot], surf.ddimmed[slot], now);
            dloss = ph == SURF_LOSS_GRACE || ph == SURF_LOSS_DIM_DUE || ph == SURF_LOSS_DIMMED;
        }
        surf_unlock();
        const bool has = used && surf_has_display(sl->model);
        const bool od_here = od_on[slot]; /* every other deck: as without on demand */
        const bool box_drives = surf.dock ? !companion : !live;
        /* Companion lost a moment ago: its images stay (Satellite: those kept; dock mode: per deck), unless a key was
         * pressed on the deck meanwhile (surf_input_route's WAKE): then the box's screens */
        const bool loss = surf.dock ? dloss : grace && any_last;
        surf_lock();
        if (!loss) surf.woke[slot] = false; /* the phase is over: the next one starts unwoken */
        const bool keep = loss && !surf.woke[slot];
        surf.keeping[slot] = has && box_drives && keep && !msg && !od_here; /* a press now wakes the box's screens */
        surf_unlock();
        const bool want = has && (od_here || (box_drives && !msg && !keep));
        if (!want) {
            /* ... and come back, as on the other decks */
            if (u->shown) ui_stop(slot, has && box_drives && keep && !msg, now);
            continue;
        }
        if (!u->shown || u->deck != sl->deck || u->m != sl->model) {
            if (u->shown) ui_stop(slot, false, now);
            ui_start(slot, sl, gen, now);
        } else if (u->gen != gen) { /* Companion took or gave back the deck, or it was redrawn anew */
            u->gen = gen;
            u->bright = -1; /* Companion may have set its own */
            memset(u->fp, 0, sizeof u->fp);
            dui_nav_cancel(&u->nav);
            dui_pin_cancel(&u->pin); /* a pad key held across it: its release may never come */
            dui_ipe_cancel(&u->ipe);
            surf_lock();
            surf.ui[slot] = true;
            surf_unlock();
        }
        const bool over_companion = od_here && companion;
        surf_lock();
        const bool was_od = surf.ui_od[slot];
        surf.ui_od[slot] = over_companion;
        if (over_companion) surf.handback[slot] = false; /* reopened during a hand-back */
        surf_unlock();
        if (over_companion && !was_od && !surf.dock) sat_release_all(slot);
        const bool own_toast_over = u->toast_until_us && now >= u->toast_until_us;
        if (own_toast_over) u->toast_until_us = 0; /* this deck's notice is over: a new snapshot below */
        if (u->od != od_here) { /* on demand opened on it (home, with Close) or closed */
            if (!od_here) pin_session_over(slot, now); /* closing ends the pad and the unlock */
            dui_nav_out_t o;
            dui_nav_ondemand(&u->nav, od_here, now_ms_of(now), &o);
            u->od = od_here;
            dui_box_t b;
            snapshot(slot, &s_snap, &b);
            u->box = b;
            apply_nav(slot, &o, false);
        } else if (snap || own_toast_over) {
            dui_box_t b;
            snapshot(slot, &s_snap, &b);
            if (memcmp(&b, &u->box, sizeof b)) {
                u->box = b;
                dui_nav_out_t o;
                dui_nav_box_changed(&u->nav, &o);
                apply_nav(slot, &o, false);
            }
        }
        if (!u->shown) continue; /* closed by the navigation just now */
        if (u->pin.open) { /* the pad's time (its idle cancel, the countdown); the screen under it waits */
            dui_pin_out_t po;
            dui_pin_tick(&u->pin, now_ms_of(now), &po);
            if (po.redraw || po.closed || po.sound) apply_pin(slot, &po, now);
        } else if (u->ipe.open) { /* the editor's time (its idle cancel, the holds, the repeats) */
            dui_ipe_out_t eo;
            dui_ipe_tick(&u->ipe, now_ms_of(now), &eo);
            if (eo.redraw || eo.closed || eo.sound || eo.save) apply_ipe(slot, &eo, now);
        } else {
            dui_nav_out_t o;
            dui_nav_tick(&u->nav, now_ms_of(now), &o);
            apply_nav(slot, &o, true);
        }
        if (!u->shown) continue;
        if (dui_unlock_step(&u->unlock, now_ms_of(now), false, u->nav.idle)) { /* 60 s idle, or 5 min */
            ESP_LOGI(TAG, "slot %d: the PIN unlock ended (%s)", slot, u->nav.idle ? "60 s without a press" : "5 minutes");
            lock_changed(slot);
        }
        ui_brightness(slot, u->box.brightness); /* on demand over Companion too */
        full &= ui_render(slot, now) == 0;
        any = true;
    }
    /* each deck's input goes back to Companion once its own hand-back is over */
    const bool handing_back = handback_step(now);
    dock_hold();
    if (any && full && !s_full_at_us) s_full_at_us = now;
    if (s_full_at_us && !s_stack_logged && now - s_full_at_us >= STACK_LOG_US) {
        s_stack_logged = true;
        ESP_LOGI(TAG, "maintenance screens drawn: stack never used: surfaces %u bytes, imgwork %u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)(surf.img_task ? uxTaskGetStackHighWaterMark(surf.img_task) : 0));
    }
    for (int i = 0; i < SURF_MAX_DECKS; i++) /* closed: the message comes back on the deck it covered */
        if (s_ui[i] && !s_ui[i]->shown && s_ui[i]->od) {
            s_ui[i]->od = false;
            surf.msg_seq_on[i] = 0;
        }
    return any || handing_back; /* the hand-back is looked at every UI_TICK_MS too */
}

void maintui_init(void)
{
    if (!s_hold_mtx) s_hold_mtx = xSemaphoreCreateMutex();
    if (!s_hold_mtx) ESP_LOGE(TAG, "no memory for the dock input hold's lock");
    if (!s_st) s_st = heap_caps_calloc(1, sizeof *s_st, MALLOC_CAP_SPIRAM);
    if (!s_st) s_st = heap_caps_calloc(1, sizeof *s_st, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_st) ESP_LOGE(TAG, "no memory for the maintenance screens' state: no screens");
    for (int i = 0; i < SURF_MAX_DECKS; i++) {
        s_ui[i] = heap_caps_calloc(1, sizeof *s_ui[i], MALLOC_CAP_SPIRAM);
        if (!s_ui[i]) ESP_LOGE(TAG, "no memory for slot %d's maintenance screen", i);
    }
}
