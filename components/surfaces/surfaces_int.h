#pragma once
/* Between surfaces.c (the wiring: registry, imgwork, Companion, the message) and maintui.c (the
 * maintenance screens on the decks). ESP-only, private to the component. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "deckmodel/model.h"
#include "deckproto/corner.h"
#include "deckproto/deckproto.h"
#include "maintmap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "problems.h"
#include "statusui/statusui.h"
#include "surfaces/surfaces.h"
#include "usbdeck/usbdeck.h"

#define SAT_DOWN_MAX 8   /* Satellite: controls held down that Companion knows of, per deck */
/* shared by surfaces.c and maintui.c */
#define STATUS_AFTER_US SURF_LOSS_OVER_US /* Companion lost this long: the maintenance screens take over */
#define TOAST_US (2200 * 1000LL)         /* a notice on the decks this long (the box-wide one and a deck's own) */
#define DEVICE_ID_PREFIX "streamdeck:"   /* + serial */
/* The serial of a device id: id without DEVICE_ID_PREFIX (id itself when it has none). */
static inline const char *surf_serial_of(const char *id)
{
    return strncmp(id, DEVICE_ID_PREFIX, strlen(DEVICE_ID_PREFIX)) ? id : id + strlen(DEVICE_ID_PREFIX);
}
#define SURF_DIM_PCT 40  /* Companion lost: its last images dimmed to this */

enum { USER_IMG, USER_SAT, USER_SURF, USER_N }; /* tasks that use deck handles outside the lock */

typedef enum { JOB_B64, JOB_RGB, JOB_SOLID, JOB_LAST, JOB_MSG } job_kind_t;

typedef struct {
    uint8_t kind;
    bool local;      /* drawn by the box (status, dim, black, message): never replaces a Companion image */
    bool overlay;    /* JOB_LAST: "Reconnecting" drawn over it (LCD keys) */
    bool msg;        /* Companion job that took over a box-drawn frame: draws the active message */
    uint8_t r, g, b; /* JOB_SOLID */
    uint8_t pct;     /* JOB_LAST: 100 = as received, less = dimmed; black when nothing is kept */
    uint32_t gen;    /* slot generation the job was made for */
    size_t len;
    uint8_t data[];  /* JOB_B64: base64 text; JOB_RGB: w*h*3 pixels at the control's size */
} job_t;


typedef struct {
    SemaphoreHandle_t mtx;
    QueueHandle_t evq;
    TaskHandle_t surf_task, img_task;
    atomic_bool poke_pending;
    bool dock; /* Network Dock mode: set before the tasks start */
    int limit; /* the decks taken at once (2 or 4; reg.cap), set before the tasks start */
    uint8_t *src, *xf, *bmp; /* imgwork scratch (PSRAM) */

    /* --- guarded by mtx --- */
    surf_registry_t reg;
    const usbdeck_t *ignored[SURF_MAX_DECKS]; /* decks over the limit, attached and ignored (compared only) */
    char ignored_sn[SURF_MAX_DECKS][33];      /* ... their serials */
    int nignored;
    coalesce_t co;
    uint32_t gen[SURF_MAX_DECKS];                    /* bumped when a slot's deck goes or a new one comes */
    uint8_t *last[SURF_MAX_DECKS][SURF_MAX_CTLS];    /* last Companion image per control, untransformed */
    usbdeck_t *inuse[USER_N];
    bool live;            /* Companion session READY: set by on_ready, cleared by the surfaces task */
    uint32_t ready_gen;   /* bumped by every on_ready */
    uint32_t handled_gen; /* ready_gen whose decks the surfaces task has (re)added */
    bool too_old;         /* last session refused as too old (until READY or another target) */
    char too_old_host[64];
    sel_result_t sel;
    char ap_ssid[33], ap_pass[65];
    bool ap_shown;  /* surfaces_set_ap_info had an SSID: the screens show the hotspot while it is on */
    bool ap_open;   /* ... with no password */
    surf_problems_t problems;
    surf_message_t msg;             /* custom message state (until_us in esp_timer time) */
    char msg_text[SURF_MSG_MAX + 1];
    uint8_t msg_page[SURF_MAX_DECKS]; /* the message's page imgwork draws on the slot's deck */
    bool owned[SURF_MAX_DECKS];     /* dock mode: Companion drives the slot's deck */
    char peer[SURF_MAX_DECKS][16];  /* dock mode: the address of the Companion that drove it last ("" none) */
    bool lost[SURF_MAX_DECKS];      /* dock mode: Companion let go of the slot's deck, still plugged in */
    bool sat_lost;                  /* Satellite: the session ended after READY, no READY since */
    bool ui[SURF_MAX_DECKS];        /* the slot's deck shows the maintenance UI: its input goes to the box */
    bool ui_od[SURF_MAX_DECKS];     /* ... on demand over Companion: Companion's images go to the cache only */
    char toast[40];                 /* surfaces_toast, until toast_until_us */
    uint8_t toast_state;            /* ... its outcome (DUI_STATE_OK / WAIT / BAD) */
    int64_t toast_until_us;
    surf_hooks_t hooks;
    surf_settings_t settings;       /* surfaces_set_settings: what the screens show of the settings */
    bool handback[SURF_MAX_DECKS];  /* the screen on demand left this deck, Companion's images not back yet:
                                     * its input goes to nobody (until handback_until at the latest) */
    int64_t handback_until[SURF_MAX_DECKS];
    bool img_at[SURF_MAX_DECKS];    /* imgwork took a job of the slot and hasn't handed it to usbdeck yet */
    uint8_t rgb[SURF_MAX_DECKS][SURF_MAX_CTLS][3]; /* Companion's colour per colour key (rgb_set) */
    bool rgb_set[SURF_MAX_DECKS][SURF_MAX_CTLS];
    atomic_bool ondemand;           /* the box menu open on any deck: read lock-free, set under mtx with od */
    surf_od_set_t od;               /* mtx: the decks it is open on (each on its own), by device_id */
    uint32_t att_seq[SURF_MAX_DECKS]; /* when the slot's deck attached (att_next then): the earliest */
    int16_t comp_bright[SURF_MAX_DECKS]; /* Satellite: Companion's last brightness for the slot's deck (-1 none) */
    uint32_t att_next;
    atomic_bool snap_dirty;         /* the maintenance screens' snapshot is out of date */

    /* --- inmtx (Satellite) --- */
    SemaphoreHandle_t inmtx;
    char sat_down[SURF_MAX_DECKS][SAT_DOWN_MAX][8]; /* control ids Companion saw pressed, per slot */
    uint8_t nsat_down[SURF_MAX_DECKS];
    corner_t corner[SURF_MAX_DECKS];              /* the gesture per slot (Satellite) */
    /* the gesture as recorded by the small tasks (docktxK, the decks' tasks), acted on by the surfaces task */
    atomic_uint corner_open_slots;                /* Satellite: slots whose gesture was made (bit per slot) */
    atomic_uintptr_t corner_open_deck;            /* dock mode: the deck whose gesture was made (0: none) */
    atomic_uint corner_clicks;                    /* dock mode: corner presses passed on late, to click for */
    atomic_uint maint_toggles;                    /* surfaces_maint_toggle: presses not acted on yet */
    const usbdeck_t *corner_deck[SURF_MAX_DECKS]; /* ... the deck it is for (compared only) */
    bool gesture_given;                           /* surfaces_set_settings ran (its gesture logged), mtx */
    atomic_bool gesture_dirty;                    /* the docks' gates to be given surf.settings.gesture */

    /* --- surfaces task only --- */
    int64_t lost_at_us; /* Companion lost at; 0 = not in the lost grace period */
    bool dimmed;
    /* dock mode, per deck (each has its own connection): Companion let go of it at (0: not in the loss phase), the
     * dim phase drawn on it, and its last key images until they are drawn dimmed (mtx) */
    int64_t dlost_at_us[SURF_MAX_DECKS];
    bool ddimmed[SURF_MAX_DECKS];
    struct surf_dock_snap *dsnap[SURF_MAX_DECKS];
    /* mtx: the box keeps Companion's last images on the deck (the loss phase, both modes; maintui sets it), and a
     * press on it woke the box's screens meanwhile (surf_input_route's WAKE; cleared once the loss phase is over) */
    bool keeping[SURF_MAX_DECKS];
    bool woke[SURF_MAX_DECKS];
    uint32_t msg_seq_on[SURF_MAX_DECKS]; /* msg.seq / gen the slot's message frames were queued for */
    uint32_t msg_gen_on[SURF_MAX_DECKS];
    sui_pager_t msg_pg[SURF_MAX_DECKS];  /* the message's page queued there */
    int64_t page_due_us;                 /* the next page of a screen shown is due (0: none) */
} surf_t;

extern surf_t surf;

/* ---- surfaces.c, for maintui.c ---- */
void surf_lock(void);
void surf_unlock(void);
job_t *surf_job_new(job_kind_t kind, size_t len, uint32_t gen, bool local);
void surf_job_free(void *p);
/* JOB_LAST: the kept image at pct (black when none), "Reconnecting" over it with overlay. */
job_t *surf_last_job(uint32_t gen, uint8_t pct, bool overlay);
/* A box-drawn image: never over a pending Companion image; offline_only refuses it while Companion is
 * live. Returns true when queued. */
bool surf_put_local(int slot, int idx, job_t *j, bool offline_only);
void surf_kick_imgwork(void);
/* The slot's deck handle for a call outside the lock (NULL: gone or another generation); release it. */
usbdeck_t *surf_deck_acquire(int user, int slot, uint32_t gen, bool *owned);
void surf_deck_release(int user);
bool surf_has_display(const deck_model_t *m);
bool surf_is_image_ctl(deck_ctl_kind_t k);
/* mtx held: the registry as the candidates (c has SURF_MAX_DECKS entries, pointing into the registry). */
void surf_od_cands_locked(surf_od_cand_t *c);
/* mtx held: the box menu is open on the slot's deck. */
bool surf_od_slot_locked(int slot);

/* ---- maintui.c, for surfaces.c ---- */
/* Allocations (the screens' state in PSRAM). Before the surfaces task starts. */
void maintui_init(void);
/* One pass of the surfaces task: which decks show the screens, their snapshot, navigation, frames,
 * the hand-back after on demand closes. msg: a custom message is shown. True while a deck shows them or
 * hands back (the surfaces task then runs a pass every UI_TICK_MS). */
bool maintui_update(int64_t now, bool msg);
/* The deck's input, posted by on_input for its screen. */
void maintui_input(int slot, uint32_t gen, const deck_event_t *ev);
/* A PIN check's answer (surfaces_pin_result) for the slot's pad: SURF_PIN_*, for the check ticket. */
void maintui_pin_result(int slot, uint32_t ticket, int result, int wait_s);
/* The slot's screen is drawn again whole (its frames were lost or replaced); false when it shows none. */
bool maintui_redraw(int slot);
/* surfaces_maint: in dock mode the chosen deck's input is held from Companion at once on opening. */
void maintui_maint(bool on);
