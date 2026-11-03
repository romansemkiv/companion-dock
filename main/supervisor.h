/* The supervisor: the one place that turns the box config and what the network sees into actions.
 * A task (1 s tick, internal stack, task watchdog) that
 *  - picks the Companion (discovery_select -> satclient_set_target + surfaces_set_selection) and
 *    stores a new pin, or the pinned instance's new address, in the config (saved to NVS when it
 *    changed, at most once a minute, and not while boxconfig keeps an unreadable stored config); not
 *    in Network Dock mode, where Companion connects to the box;
 *  - applies config changes signalled with supervisor_config_changed (netmgr_apply,
 *    discovery_apply, the hotspot credentials on the status screens);
 *  - tells the status screens about network changes (NETMGR_EVENT_CHANGED -> surfaces_net_changed)
 *    and gives them the hotspot SSID and password while the hotspot is on;
 *  - reports what the box is doing to the status light and sounds (feedback_status: network,
 *    setup hotspot, Companion, deck trouble, a firmware upload) and hands them their settings;
 *  - runs the OTA self-check of a new image (30 s after start, see supervisor.c);
 *  - restarts the box when a lost C6 leaves it without a network (supervisor.c).
 * netmgr_apply and discovery_apply are only ever called from this task. */
#pragma once
#include "boxconfig/config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* *cfg is the box's one config for the whole firmware run (it must outlive everything). Call once,
 * right after it is loaded and before anything else may touch it (webui). mode: the surface mode
 * this run uses (read once at boot; cfg->surface_mode may change before the restart). */
void supervisor_init(box_config_t *cfg, box_surface_mode_t mode);

/* Starts the task. The last boot stage: the OTA self-check counts its 30 s from here. */
esp_err_t supervisor_start(void);

/* The config was changed and saved (webui): the supervisor applies it within a second.
 * Never blocks; callable from any task, also before supervisor_start (applied on its first tick). */
void supervisor_config_changed(void);

/* Guards the config given to supervisor_init. The supervisor reads it under this lock and writes
 * companion_pin_id / companion_host / companion_port (then saves) under it; every other writer holds
 * it while changing the config and saving it. Not recursive; never held across a call that waits on
 * the supervisor. */
void supervisor_cfg_lock(void);
void supervisor_cfg_unlock(void);

#ifdef __cplusplus
}
#endif
