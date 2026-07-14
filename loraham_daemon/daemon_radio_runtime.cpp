#include "daemon_radio_runtime.h"

#include <stdio.h>

#include "daemon_log.h"
#include "daemon_radio_selection.h"
#include "hardware_profile.h"
#include "daemon_stats.h"
#include "daemon_tx_async_runtime.h"
#include "radio_health.h"

/* --- Radio controller state --------------------------------------------- */

RadioController radio_controller_433;
RadioController radio_controller_868;

/* --- Radio controller setup --------------------------------------------- */

void daemon_radio_controller_init(void)
{
    daemon_debug_ctx("RADIO", "Controller initialisieren");

    daemon_tx_async_runtime_init();

    radio_controller_init(&radio_controller_433,
                          RADIO_BAND_433,
                          "433",
                          false,
                          setFlag433,
                          daemon_led_pin_433_configured());
    daemon_debug_band("433", "Controller bereit");

    radio_controller_init(&radio_controller_868,
                          RADIO_BAND_868,
                          "868",
                          true,
                          setFlag868,
                          daemon_led_pin_868_configured());
    daemon_debug_band("868", "Controller bereit");

    /* Hardware capability from the resolved profile (selected band only;
     * the other controller stays at its default and is never used). */
    if (daemon_radio_433_enabled())
        radio_controller_433.cad_scan_available =
            daemon_hw_profile.cad_scan_available;
    if (daemon_radio_868_enabled())
        radio_controller_868.cad_scan_available =
            daemon_hw_profile.cad_scan_available;
}

/* --- Radio controller shutdown ------------------------------------------ */

static void radio_controller_shutdown(RadioController *ctrl)
{
    const char *tag;

    if (!ctrl)
        return;

    tag = radio_controller_tag(ctrl);
    daemon_debug_ctx(tag, "Radio-Shutdown");

    if (ctrl->driver) {
        if (radio_controller_ready(ctrl)) {
            daemon_debug_band(tag, "Callback aus");
            ctrl->driver->clearPacketReceivedAction();
            daemon_debug_band(tag, "Standby");
            ctrl->driver->standby();
            daemon_debug_band(tag, "IRQ löschen");
            ctrl->driver->clearIrq(0xFFFFFFFF);
        } else {
            daemon_debug_band(tag, "Radio nicht bereit");
        }

        daemon_debug_band(tag, "Radio freigeben");
        ctrl->driver.reset();
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
    daemon_tx_async_runtime_shutdown();

    if (daemon_radio_433_enabled()) {
        daemon_debug_ctx("RADIO", "Shutdown 433");
        radio_controller_shutdown(&radio_controller_433);
    }

    if (daemon_radio_868_enabled()) {
        daemon_debug_ctx("RADIO", "Shutdown 868");
        radio_controller_shutdown(&radio_controller_868);
    }

    daemon_led_shutdown();
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
