#ifndef LORAHAM_RADIO_CONTROLLER_H
#define LORAHAM_RADIO_CONTROLLER_H

#include <memory>
#include <atomic>
#include <mutex>

#include "hal/RPi/PiHal.h"
#include <RadioLib.h>

#include "daemon_tx_policy.h"
#include "radio_channel.h"
#include "daemon_stats.h"
#include "radio_health.h"

/* --- Radio hardware/runtime state --------------------------------------- */

typedef enum {
    RADIO_TX_MODE_MANAGED = 0,
    RADIO_TX_MODE_DIRECT = 1
} RadioTxMode_t;

static inline const char *radio_tx_mode_name(RadioTxMode_t mode)
{
    return mode == RADIO_TX_MODE_DIRECT ? "DIRECT" : "MANAGED";
}

static inline const char *radio_mode_name(RadioMode_t mode)
{
    return mode == RADIO_MODE_FSK ? "FSK" : "LORA";
}

template<typename RadioT>
struct RadioController {
    RadioBand_t band;
    const char *tag;
    bool is_hf;

    std::unique_ptr<PiHal> hal;
    std::unique_ptr<Module> mod;
    std::unique_ptr<RadioT> radio;
    std::recursive_mutex radio_mutex;

    RadioHealth health;
    RadioMode_t mode;
    std::atomic<bool> received;
    std::atomic<bool> tx_busy;
    std::atomic<uint32_t> tx_status_generation;
    uint32_t tx_status_broadcast_generation;
    std::atomic<bool> cad_active;
    std::atomic<bool> cad_broadcast_active;
    std::atomic<bool> cad_monitor_active;
    std::atomic<bool> getrssi_active;
    std::atomic<bool> tx_result_active;
    std::atomic<bool> tx_queue_active;
    RadioTxMode_t tx_mode;
    uint16_t tx_result_seq;

    std::atomic<uint32_t> cad_wait_timeout_ms;
    std::atomic<uint32_t> cad_idle_stable_ms;
    std::atomic<uint32_t> cad_poll_interval_ms;
    std::atomic<bool>     cad_send_after_timeout;

    DaemonRadioStats stats;

    void (*rx_callback)(void);

    int led_pin;
    std::mutex led_mutex;
    int led_state;
};

template<typename RadioT>
static inline void radio_controller_init(RadioController<RadioT> *ctrl,
                                         RadioBand_t band,
                                         const char *tag,
                                         bool is_hf,
                                         void (*rx_callback)(void),
                                         int led_pin)
{
    if (!ctrl)
        return;

    ctrl->band = band;
    ctrl->tag = tag;
    ctrl->is_hf = is_hf;

    ctrl->hal.reset();
    ctrl->mod.reset();
    ctrl->radio.reset();

    ctrl->health = RADIO_HEALTH_UNINITIALIZED;
    ctrl->mode = RADIO_MODE_LORA;
    ctrl->received.store(false);
    ctrl->tx_busy.store(false);
    ctrl->tx_status_generation.store(0u);
    ctrl->tx_status_broadcast_generation = 0u;
    ctrl->cad_active.store(false);
    ctrl->cad_broadcast_active.store(false);
    ctrl->cad_monitor_active.store(false);
    ctrl->getrssi_active.store(false);
    ctrl->tx_result_active.store(false);
    ctrl->tx_queue_active.store(true);
    ctrl->tx_mode = RADIO_TX_MODE_MANAGED;
    ctrl->tx_result_seq = 0;

    ctrl->cad_wait_timeout_ms.store(DAEMON_TX_POLICY_CAD_WAIT_TIMEOUT_MS);
    ctrl->cad_idle_stable_ms.store(DAEMON_TX_POLICY_CAD_IDLE_STABLE_MS);
    ctrl->cad_poll_interval_ms.store(DAEMON_TX_POLICY_POLL_INTERVAL_MS);
    ctrl->cad_send_after_timeout.store(
        DAEMON_TX_POLICY_SEND_AFTER_CAD_TIMEOUT ? true : false);

    daemon_radio_stats_init(&ctrl->stats);

    ctrl->rx_callback = rx_callback;
    ctrl->led_pin = led_pin;
    ctrl->led_state = -1;
}

template<typename RadioT>
static inline int radio_controller_band_number(const RadioController<RadioT> *ctrl)
{
    return ctrl ? (int)ctrl->band : 0;
}

template<typename RadioT>
static inline const char *radio_controller_tag(const RadioController<RadioT> *ctrl)
{
    if (!ctrl || !ctrl->tag)
        return "?";

    return ctrl->tag;
}

template<typename RadioT>
static inline RadioHealth *radio_controller_health_ptr(RadioController<RadioT> *ctrl)
{
    return ctrl ? &ctrl->health : nullptr;
}

template<typename RadioT>
static inline RadioHealth radio_controller_health(const RadioController<RadioT> *ctrl)
{
    return ctrl ? ctrl->health : RADIO_HEALTH_FAILED;
}

template<typename RadioT>
static inline bool radio_controller_ready(const RadioController<RadioT> *ctrl)
{
    return radio_health_is_ready(radio_controller_health(ctrl));
}

template<typename RadioT>
static inline RadioMode_t radio_controller_mode(const RadioController<RadioT> *ctrl)
{
    return ctrl ? ctrl->mode : RADIO_MODE_LORA;
}

template<typename RadioT>
static inline float radio_controller_packet_rssi(RadioController<RadioT> *ctrl)
{
    if (!ctrl || !ctrl->radio)
        return -200.0f;

    std::lock_guard<std::recursive_mutex> radio_lock(ctrl->radio_mutex);
    return ctrl->radio->getRSSI();
}

#endif
