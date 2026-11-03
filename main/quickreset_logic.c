#include "quickreset_logic.h"

quickreset_action_t quickreset_on_start(uint8_t *count)
{
    if (*count < 255) (*count)++;
    return *count == 3 ? QR_SETUP : QR_NONE;
}
