#include "daemon_radio_runtime.h"

#include <stdio.h>

#include "daemon_band.h"
#include "daemon_log.h"
#include "hardware_profile.h"
#include "daemon_stats.h"
#include "daemon_tx_async_runtime.h"
#include "radio_health.h"

/* --- Radio controller state --------------------------------------------- */

RadioController radio_controller;

/* --- Radio controller setup --------------------------------------------- */

void daemon_radio_controller_init(void)
{
    const DaemonBandDescriptor *band = daemon_band();

    daemon_debug_ctx("RADIO", "Controller initialisieren");

    daemon_tx_async_runtime_init();

    radio_controller_init(&radio_controller,
                          band->band,
                          band->tag,
                          band->is_hf,
                          setFlag,
                          daemon_led_pin_configured());
    daemon_debug_band(band->tag, "Controller bereit");

    /* Hardware capability from the resolved profile. */
    radio_controller.cad_scan_available = daemon_hw_profile.cad_scan_available;
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

    daemon_debug_ctx("RADIO", "Shutdown %s", daemon_band()->tag);
    radio_controller_shutdown(&radio_controller);

    daemon_led_shutdown();
}

/* --- RX callback ---------------------------------------------------------- */

void setFlag(void)
{
    radio_controller.received.store(true);
}

/* --- Active radio state -------------------------------------------------- */

bool daemon_selected_radio_ready(void)
{
    return radio_controller_ready(&radio_controller);
}

void daemon_log_active_radios(void)
{
    if (radio_controller_ready(&radio_controller))
        printf("[Daemon] Aktive Radios: %s\n", daemon_band()->tag);
    else
        printf("[Daemon] Aktive Radios: none\n");
}
