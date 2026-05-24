#ifndef LORAHAM_DAEMON_RX_H
#define LORAHAM_DAEMON_RX_H

#include <stdint.h>

#include "daemon_protocol.h"

/* --- RX runtime helpers -------------------------------------------------- */

void daemon_process_radio_433(uint8_t (&rx_buf_433)[buf_SIZE]);
void daemon_process_radio_868(uint8_t (&rx_buf_868)[buf_SIZE]);

#endif
