/* The words of the RPC refusals the decks turn into two-word reasons (deckui dui_refusal): set here once,
 * used where each refusal is made (slotsync_err_text, the web methods) and where it is read, so a reworded refusal
 * changes both ends. Pure: no includes. */
#pragma once

#define RPC_MSG_UPLOAD "a firmware upload is running, or the box is restarting into the other slot"
#define RPC_MSG_SYNCING "the recovery slot is being synced (about a minute): try again shortly"
#define RPC_MSG_CHECKING "the firmware slots are still being checked: try again in a few seconds"
#define RPC_MSG_SELF_CHECK "the running firmware is still checking itself (about 30 s after start): try again shortly"
#define RPC_MSG_NEWER "the other slot holds a newer firmware: sync with force to replace it"
#define RPC_MSG_EMPTY "the other slot holds no firmware"
#define RPC_MSG_INVALID "the firmware in the other slot is invalid"
#define RPC_MSG_FOREIGN "the other slot holds another project's firmware: it can't be started from here"
#define RPC_MSG_RESTARTING "a restart is already on its way"
#define RPC_MSG_NO_BACKUP "no backup network is set up"
#define RPC_MSG_WIFI_OFF "Wi-Fi isn't running"
#define RPC_MSG_NO_DECK "No Stream Deck with a screen is attached to show it on"
#define RPC_MSG_NO_MEMORY "no memory"

/* The codes the decks read first: their one definition (rpc/rpc.h includes this; it needs no cJSON, for deckui). */
#define RPC_E_INVALID_ARG (-103)
#define RPC_E_BAD_STATE (-104)
#define RPC_E_NOT_FOUND (-105)
#define RPC_E_UNAVAILABLE (-114)
#define RPC_E_UNAUTHORIZED (401)
