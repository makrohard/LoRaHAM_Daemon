#include "daemon_radio_runtime.h"

#include <stdio.h>

#include "daemon_log.h"
#include "daemon_radio_selection.h"
#include "daemon_stats.h"
#include "radio_health.h"

/* --- Radio controller state --------------------------------------------- */

RadioController<SX1278> radio_controller_433;
RadioController<RFM95> radio_controller_868;

/* --- Radio controller setup --------------------------------------------- */

void daemon_radio_controller_init(void)
{
    daemon_debug_ctx("RADIO", "Controller initialisieren");

    radio_controller_init(&radio_controller_433,
                          RADIO_BAND_433,
                          "433",
                          false,
                          setFlag433,
                          DAEMON_LED_PIN_433);
    daemon_debug_band("433", "Controller bereit");

    radio_controller_init(&radio_controller_868,
                          RADIO_BAND_868,
                          "868",
                          true,
                          setFlag868,
                          DAEMON_LED_PIN_868);
    daemon_debug_band("868", "Controller bereit");
}

/* --- Radio controller shutdown ------------------------------------------ */

template<typename RadioT>
static void radio_controller_shutdown(RadioController<RadioT> *ctrl)
{
    const char *tag;

    if (!ctrl)
        return;

    tag = radio_controller_tag(ctrl);
    daemon_debug_ctx(tag, "Radio-Shutdown");

    if (ctrl->radio) {
        if (radio_controller_ready(ctrl)) {
            daemon_debug_band(tag, "Callback aus");
            ctrl->radio->clearPacketReceivedAction();
            daemon_debug_band(tag, "Standby");
            ctrl->radio->standby();
            daemon_debug_band(tag, "IRQ löschen");
            ctrl->radio->clearIrq(0xFFFFFFFF);
        } else {
            daemon_debug_band(tag, "Radio nicht bereit");
        }

        daemon_debug_band(tag, "Radio freigeben");
        ctrl->radio.reset();
    } else {
        daemon_debug_band(tag, "Kein Radio-Objekt");
    }

    daemon_debug_band(tag, "Modul freigeben");
    ctrl->mod.reset();
    daemon_debug_band(tag, "HAL freigeben");
    ctrl->hal.reset();

    ctrl->health = RADIO_HEALTH_UNINITIALIZED;
    ctrl->received.store(false);
    ctrl->tx_busy.store(false);
    ctrl->cad_active.store(false);
    ctrl->getrssi_active.store(false);
    daemon_radio_stats_init(&ctrl->stats);
    daemon_debug_band(tag, "Zustand zurückgesetzt");
}

void daemon_radio_shutdown_cleanup(void)
{
    if (daemon_radio_433_enabled()) {
        daemon_debug_ctx("RADIO", "Shutdown 433");
        radio_controller_shutdown(&radio_controller_433);
    }

    if (daemon_radio_868_enabled()) {
        daemon_debug_ctx("RADIO", "Shutdown 868");
        radio_controller_shutdown(&radio_controller_868);
    }
}

/* --- RX callbacks -------------------------------------------------------- */

void setFlag868(void)
{
    radio_controller_868.received.store(true);
}

void setFlag433(void)
{
    radio_controller_433.received.store(true);
}

void setFlashFlag868(void)
{
    daemon_radio_runtime_flash_led(&radio_controller_868);
}

void setFlashFlag433(void)
{
    daemon_radio_runtime_flash_led(&radio_controller_433);
}

/* --- Active radio state -------------------------------------------------- */

bool daemon_selected_radio_ready(void)
{
    if (daemon_radio_433_enabled() && radio_controller_ready(&radio_controller_433))
        return true;

    if (daemon_radio_868_enabled() && radio_controller_ready(&radio_controller_868))
        return true;

    return false;
}

void daemon_log_active_radios(void)
{
    bool active_433 = daemon_radio_433_enabled() &&
                      radio_controller_ready(&radio_controller_433);
    bool active_868 = daemon_radio_868_enabled() &&
                      radio_controller_ready(&radio_controller_868);

    if (active_433 && active_868) {
        printf("[Daemon] Aktive Radios: 433, 868\n");
    } else if (active_433) {
        printf("[Daemon] Aktive Radios: 433\n");
    } else if (active_868) {
        printf("[Daemon] Aktive Radios: 868\n");
    } else {
        printf("[Daemon] Aktive Radios: none\n");
    }
}
