#pragma once

#include "stdint.h"

/*
 * USB subsystem glue.
 *
 * Keeps polling-based USB drivers (keyboard, usb-net) behind a single init/poll API.
 */

#ifdef __cplusplus
extern "C" {
#endif

void usb_init(void);
void usb_poll(void);

#ifdef __cplusplus
}
#endif
