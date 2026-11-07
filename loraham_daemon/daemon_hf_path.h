#ifndef LORAHAM_DAEMON_HF_PATH_H
#define LORAHAM_DAEMON_HF_PATH_H

#include <stddef.h>
#include <stdint.h>

#include "radio_channel.h"

/* --- The HF path ------------------------------------------------------------ */
/*
 * What 868 and 915 do and 433 does not: after an RX read the packet buffer is
 * cleared, and after a TX the daemon waits DAEMON_TX_GUARD_US before it
 * restores RX. Before 915 these were "band == 868" in daemon_rx.cpp and
 * daemon_tx.cpp.
 */

#define DAEMON_TX_GUARD_US 50000u

bool daemon_hf_path(RadioBand_t band);
void daemon_rx_clear_after_read(RadioBand_t band, uint8_t *buf, size_t len);
unsigned daemon_tx_guard_us(RadioBand_t band);
void daemon_tx_guard_wait(RadioBand_t band);

/* Tests only: the wait daemon_tx_guard_wait uses (usleep by default). */
void daemon_tx_guard_set_sleep(void (*sleep_us)(unsigned));

#endif
