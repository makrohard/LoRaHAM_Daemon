#ifndef LORAHAM_DAEMON_RADIO_RUNTIME_H
#define LORAHAM_DAEMON_RADIO_RUNTIME_H

#include "daemon_led.h"
#include "daemon_tx_async_runtime.h"
#include "radio_controller.h"

/* --- Radio runtime helpers ---------------------------------------------- */

extern RadioController<SX1278> radio_controller_433;
extern RadioController<RFM95> radio_controller_868;

void daemon_radio_controller_init(void);
void daemon_radio_shutdown_cleanup(void);
bool daemon_selected_radio_ready(void);
void daemon_log_active_radios(void);

void setFlag433(void);
void setFlag868(void);

template<typename RadioT>
static inline void daemon_radio_runtime_led(RadioController<RadioT> *ctrl,
                                            int state)
{
    if (!ctrl)
        return;

    daemon_led_set_pin(ctrl->led_pin, state);
}

/*
 * Single source of truth for the per-band activity LED.
 *
 * The LED is ON whenever the band is transmitting or the channel/CAD is busy,
 * derived purely from the two atomics. Every runtime LED write goes through
 * here so the pin can never latch ON without a matching OFF.
 *
 * The TX worker thread and the main loop both reconcile the LED, so the
 * derive/cache-check/write sequence is held under led_mutex: this keeps the
 * cached led_state in lockstep with the physical pin and lets the cache safely
 * skip redundant GPIO writes. led_mutex is a leaf lock (only atomics and the
 * GPIO write happen inside it), so it introduces no lock-ordering risk.
 */
template<typename RadioT>
static inline void daemon_radio_runtime_sync_led(RadioController<RadioT> *ctrl)
{
    if (!ctrl)
        return;

    std::lock_guard<std::mutex> led_lock(ctrl->led_mutex);

    int desired = (ctrl->tx_busy.load() || ctrl->cad_broadcast_active.load())
                      ? 1 : 0;

    if (desired == ctrl->led_state)
        return;

    ctrl->led_state = desired;
    daemon_led_set_pin(ctrl->led_pin, desired);
}

#endif
