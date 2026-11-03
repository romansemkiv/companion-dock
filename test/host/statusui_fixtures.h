#pragma once
/* Test data: the status screens the box drew before the maintenance screens (statusui_fixtures.c). */
#include <stdint.h>
#include "statusui/statusui.h"

// ---- The former status screens (now test data) ----
// The words' roles: the main message in status, hints and labels in label, IPv4 octets and port
// numbers in accent, the SSID, password, versions and host in value. The state: BAD for no
// network, Companion too old and no free dock, WAIT for the others. The zones: the first line is
// the status, the next lines the main zone, the hotspot's lines the service zone (when ap_ssid is set:
// "WiFi <ssid>" and "Pass <pass>", or "No password" when ap_pass is NULL or ""). Custom messages:
// SUI_ROLE_FG and no zones, white on black. Each screen in the fewest words that still say what to do.
// "No network"; main "Setup page" and the hotspot's address (ap_ip), or "Plug in network" when the
// hotspot is off.
void fx_screen_no_network(sui_content_t *c, const char *ap_ip, const char *ap_ssid, const char *ap_pass);
// "Finding Companion" (or "Connecting" and main the target first); main "Box IP" and ip.
void fx_screen_looking(sui_content_t *c, const char *ip, const char *target,
                        const char *ap_ssid, const char *ap_pass);
// "Choose Companion"; main "Web page" and ip (the web page's address).
void fx_screen_choose(sui_content_t *c, const char *ip, const char *ap_ssid, const char *ap_pass);
// "Companion too old"; main "Needs 5.0+" and "Has <ver>".
void fx_screen_too_old(sui_content_t *c, const char *ver, const char *ap_ssid, const char *ap_pass);
// Dock mode, a deck past the sixth: "No free dock"; main "Max 6 decks".
void fx_screen_no_dock(sui_content_t *c);
// Network Dock mode (no Companion connection to this deck's dock): "Waiting for Companion"; main
// "Remote surface" and "<ip> port <port>". Every screen that shows the hotspot keeps "WiFi <ssid>" with
// "Pass <pass>" (or "No password") together, both whole.
void fx_screen_dock(sui_content_t *c, const char *ip, uint16_t port, const char *ap_ssid,
                     const char *ap_pass);
