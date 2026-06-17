#ifndef LORAHAM_RADIO_CONTROLLER_H
#define LORAHAM_RADIO_CONTROLLER_H

#include <memory>
#include <atomic>

#include "hal/RPi/PiHal.h"
#include <RadioLib.h>

#include "radio_channel.h"
#include "daemon_stats.h"
#include "radio_health.h"

/* --- Radio hardware/runtime state --------------------------------------- */

template<typename RadioT>
struct RadioController {
    RadioBand_t band;
    const char *tag;
    bool is_hf;

    std::unique_ptr<PiHal> hal;
    std::unique_ptr<Module> mod;
    std::unique_ptr<RadioT> radio;

    RadioHealth health;
    RadioMode_t mode;
    std::atomic<bool> received;
    std::atomic<bool> tx_busy;
    std::atomic<bool> cad_active;
    std::atomic<bool> getrssi_active;
    std::atomic<bool> tx_result_active;
    uint16_t tx_result_seq;

    DaemonRadioStats stats;

    void (*rx_callback)(void);

    int led_pin;
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
    ctrl->cad_active.store(false);
    ctrl->getrssi_active.store(false);
    ctrl->tx_result_active.store(false);
    ctrl->tx_result_seq = 0;

    daemon_radio_stats_init(&ctrl->stats);

    ctrl->rx_callback = rx_callback;
    ctrl->led_pin = led_pin;
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

    return ctrl->radio->getRSSI();
}

#endif
