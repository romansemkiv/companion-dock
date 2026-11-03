#include "netmgr/wififo_logic.h"

static wfo_act_t act_for(wfo_net_t n) { return n == WFO_NET_BACKUP ? WFO_ACT_CONNECT_BACKUP : WFO_ACT_CONNECT_MAIN; }
static wfo_act_t use_for(wfo_net_t n) { return n == WFO_NET_BACKUP ? WFO_ACT_USE_BACKUP : WFO_ACT_USE_MAIN; }

wfo_act_t wfo_start(wfo_t *w, int64_t now_ms, bool has_backup)
{
    w->pref = WFO_NET_MAIN; /* a new configuration: the main network first again */
    return wfo_start_on(w, now_ms, has_backup, WFO_NET_MAIN);
}

wfo_act_t wfo_start_on(wfo_t *w, int64_t now_ms, bool has_backup, wfo_net_t net)
{
    w->started = true;
    w->has_backup = has_backup;
    w->assoc = w->has_ip = false;
    w->expect_disc = 0;
    w->lost_pending = w->use_pending = false;
    w->not_found[WFO_NET_MAIN] = w->not_found[WFO_NET_BACKUP] = false;
    w->net = has_backup && net == WFO_NET_BACKUP ? WFO_NET_BACKUP : WFO_NET_MAIN;
    /* the backup chosen by hand stays the first choice over a restart on it; else the main */
    if (!(w->net == WFO_NET_BACKUP && w->pref == WFO_NET_BACKUP)) w->pref = WFO_NET_MAIN;
    w->since_ms = now_ms;
    return act_for(w->net);
}

wfo_net_t wfo_restart_net(const wfo_t *w, bool sta_same)
{
    return sta_same && w->started && w->has_backup ? w->net : WFO_NET_MAIN;
}

wfo_act_t wfo_use(wfo_t *w, int64_t now_ms, wfo_net_t net)
{
    if (!w->started || (net == WFO_NET_BACKUP && !w->has_backup)) return WFO_ACT_NONE;
    w->pref = net; /* the one in use from now on, even when nothing needs to reconnect */
    if (net == w->net) { /* already the one tried or joined: nothing to switch (a pending other choice is off) */
        w->use_pending = false;
        return WFO_ACT_NONE;
    }
    if (w->scanning) { /* the scan owns the radio: the switch when it ends; net names the old one till then */
        w->use_pending = true;
        w->use_net = net;
        return WFO_ACT_NONE;
    }
    w->net = net;
    w->since_ms = now_ms;
    w->lost_pending = w->use_pending = false;
    w->assoc = w->has_ip = false; /* the glue drops the other on purpose (an own disconnect) */
    return use_for(net);
}

void wfo_stop(wfo_t *w)
{
    w->started = false;
    w->assoc = w->has_ip = false;
    w->expect_disc = 0;
    w->scanning = false;
    w->lost_pending = w->use_pending = false;
}

bool wfo_joined(const wfo_t *w) { return w->started && w->assoc && w->has_ip; }

/* The joined network was lost at now_ms. */
static wfo_act_t lost(wfo_t *w, int64_t now_ms)
{
    w->since_ms = now_ms;
    if (w->has_backup && w->net != w->pref) { /* off the other one: the first choice (the main, or the one
                                               * chosen by hand) at once */
        w->net = w->pref;
        if (w->scanning) { /* the scan owns the radio: it ends first */
            w->lost_pending = true;
            return WFO_ACT_NONE;
        }
        return act_for(w->net);
    }
    return WFO_ACT_NONE; /* the first choice is retried for WFO_SWITCH_MS before the other */
}

wfo_act_t wfo_connected(wfo_t *w, int64_t now_ms)
{
    (void)now_ms;
    if (!w->started) return WFO_ACT_NONE;
    w->assoc = true;
    w->not_found[w->net] = false; /* found, and joined */
    w->expect_disc = 0; /* a new attempt follows the own DISCONNECTED in event order */
    if (wfo_joined(w)) w->lost_pending = false;
    return WFO_ACT_NONE;
}

wfo_act_t wfo_got_ip(wfo_t *w, int64_t now_ms)
{
    (void)now_ms;
    if (!w->started) return WFO_ACT_NONE;
    w->has_ip = true;
    if (wfo_joined(w)) w->lost_pending = false;
    return WFO_ACT_NONE;
}

wfo_act_t wfo_lost_ip(wfo_t *w, int64_t now_ms)
{
    if (!w->started) return WFO_ACT_NONE;
    const bool was = wfo_joined(w);
    w->has_ip = false;
    return was ? lost(w, now_ms) : WFO_ACT_NONE;
}

wfo_act_t wfo_disconnected(wfo_t *w, int64_t now_ms, int reason)
{
    (void)reason; /* not found, wrong password and a lost link are all "not joined" */
    if (!w->started) return WFO_ACT_NONE;
    const bool was = wfo_joined(w);
    w->assoc = w->has_ip = false;
    return was ? lost(w, now_ms) : WFO_ACT_NONE; /* else a failed attempt: the retry timer's business */
}

static wfo_act_t switch_net(wfo_t *w, int64_t now_ms)
{
    w->net = w->net == WFO_NET_MAIN ? WFO_NET_BACKUP : WFO_NET_MAIN;
    w->since_ms = now_ms;
    return act_for(w->net);
}

wfo_act_t wfo_tick(wfo_t *w, int64_t now_ms)
{
    if (w->expect_disc > 0 && now_ms - w->disc_ms >= WFO_DISC_WAIT_MS) w->expect_disc = 0;
    if (!w->started || !w->has_backup || wfo_joined(w) || w->scanning || w->lost_pending) return WFO_ACT_NONE;
    if (now_ms - w->since_ms < WFO_SWITCH_MS) return WFO_ACT_NONE;
    return switch_net(w, now_ms);
}

void wfo_scan_started(wfo_t *w) { w->scanning = true; }

wfo_act_t wfo_scan_ended(wfo_t *w, int64_t now_ms)
{
    w->scanning = false;
    if (!w->started || !w->has_backup) return WFO_ACT_NONE;
    if (w->use_pending) { /* a network chosen by hand during the scan: now, unless it is the one already */
        w->use_pending = false;
        const bool lost = w->lost_pending;
        w->lost_pending = false;
        if (w->use_net != w->net || !wfo_joined(w)) {
            const bool other = w->use_net != w->net;
            w->net = w->use_net;
            w->since_ms = now_ms;
            if (other) w->assoc = w->has_ip = false; /* the glue drops the other on purpose */
            if (other || lost) return use_for(w->net);
        }
        return WFO_ACT_NONE;
    }
    if (w->lost_pending) { /* lost during the scan: the first choice now, its 30 s start here */
        w->lost_pending = false;
        w->since_ms = now_ms;
        return act_for(w->net);
    }
    return wfo_tick(w, now_ms); /* a switch that fell due during the scan */
}

void wfo_disconnect_requested(wfo_t *w, int64_t now_ms)
{
    if (!w->started) return;
    if (w->expect_disc++ == 0) w->disc_ms = now_ms;
}

void wfo_set_net(wfo_t *w, wfo_net_t net) { w->net = net; }

void wfo_disconnect_failed(wfo_t *w)
{
    if (w->expect_disc > 0) w->expect_disc--;
}

bool wfo_own_disconnect(wfo_t *w)
{
    if (!w->started || w->expect_disc <= 0) return false;
    w->expect_disc--;
    w->assoc = w->has_ip = false; /* dropped on purpose: no loss, the switch has been decided already */
    return true;
}

bool wfo_retry_ok(const wfo_t *w) { return w->expect_disc <= 0; }

wfo_net_t wfo_net(const wfo_t *w) { return w->net; }

void wfo_note_attempt(wfo_t *w, wfo_net_t net, int reason)
{
    if (!w->started || (net != WFO_NET_MAIN && net != WFO_NET_BACKUP)) return;
    w->not_found[net] = reason == WFO_REASON_NO_AP_FOUND || reason == WFO_REASON_NO_AP_FOUND_SECURITY ||
                        reason == WFO_REASON_NO_AP_FOUND_AUTHMODE || reason == WFO_REASON_NO_AP_FOUND_RSSI;
}

bool wfo_not_found(const wfo_t *w, wfo_net_t net)
{
    return (net == WFO_NET_MAIN || net == WFO_NET_BACKUP) && w->not_found[net];
}
