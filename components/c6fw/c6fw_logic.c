#include "c6fw_logic.h"

bool c6fw_needs_update(c6ver_t running, bool known, c6ver_t carried)
{
    return !known || running.major != carried.major || running.minor != carried.minor ||
           running.patch != carried.patch;
}

c6fw_action_t c6fw_decide(c6ver_t running, bool known, c6ver_t carried, bool pending, int tries, bool no_image)
{
    if (!c6fw_needs_update(running, known, carried)) return C6FW_OK;
    if (no_image) return C6FW_MISMATCH; /* whether it still works: c6fw_usable */
    if (pending) return C6FW_WAIT_CONFIRM;
    return tries >= C6FW_MAX_TRIES ? C6FW_GIVE_UP : C6FW_UPDATE;
}

bool c6fw_usable(c6ver_t running, bool known, c6ver_t carried)
{
    return known && running.major == carried.major;
}
