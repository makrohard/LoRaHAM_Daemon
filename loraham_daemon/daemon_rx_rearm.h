#ifndef LORAHAM_DAEMON_RX_REARM_H
#define LORAHAM_DAEMON_RX_REARM_H

#include "radio_controller.h"

/* --- RX re-arm robustness (audit M3) -------------------------------------- */
/*
 * Every startReceive() result is captured; a failed re-arm must never leave a
 * READY-but-deaf radio behind silently.
 *
 * Runtime re-arms (post-TX restore, RX drain, CAD-probe restore, CONFIG):
 * daemon_rx_rearm_note_result(). On failure: one latched log line per
 * incident (no per-tick spam), the rx_rearm_failures stats counter
 * increments, and rx_rearm_pending is set. On a successful (re)arm while
 * pending: recovery is logged and the latch cleared.
 *
 * Boot: daemon_rx_rearm_boot_result() fails closed — RADIO_HEALTH_FAILED
 * with the numeric RadioLib code; returns true when RX is armed.
 *
 * Retry: daemon_rx_rearm_retry() runs in the main loop's radio tick. Lock
 * discipline (radio_controller.h): gated on tx_busy and try_to_lock, never
 * blocking behind the TX worker.
 */
void daemon_rx_rearm_note_result(RadioController *ctrl, int16_t state,
                                 const char *ctx);
bool daemon_rx_rearm_boot_result(RadioController *ctrl, int16_t state);
void daemon_rx_rearm_retry(RadioController *ctrl);

#endif
