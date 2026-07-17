#include "daemon_radio_init.h"

#include <stdio.h>

#include "hal/RPi/PiHal.h"
#include <RadioLib.h>

#include "locking_pihal.h"

#include "daemon_band.h"
#include "daemon_gpio_lock.h"
#include "daemon_led.h"
#include "daemon_log.h"
#include "daemon_radio_runtime.h"
#include "daemon_rx_rearm.h"
#include "hardware_profile.h"
#include "radio_controller.h"
#include "radio_driver.h"
#include "radio_health.h"
#include "sx127x_driver.h"
#include "sx1262_driver.h"

/* --- Profile helpers ------------------------------------------------------ */

static uint32_t hw_pin_or_nc(int pin)
{
    return pin < 0 ? RADIOLIB_NC : (uint32_t)pin;
}

/* Driver selection by profile chip family (is_hf mirrors today's band split:
 * 433 -> SX1278, 868 -> RFM95 within the SX127x family). */
static RadioDriver *hw_driver_create(Module *mod, bool is_hf)
{
    if (daemon_hw_profile.family == DAEMON_CHIP_FAMILY_SX1262)
        return sx1262_driver_create(mod,
                                    daemon_hw_profile.tcxo_voltage,
                                    daemon_hw_profile.txen);

    return sx127x_driver_create(mod, is_hf);
}

/* Family-aware D8 diagnosis dispatch (one line per failed radio). */
static void hw_diagnose_begin_failure(Module *mod, const char *band, int state)
{
    if (daemon_hw_profile.family == DAEMON_CHIP_FAMILY_SX1262) {
        sx1262_diagnose_begin_failure(band, state);
        return;
    }

    sx127x_diagnose_begin_failure(mod, band, state);
}

/* One warm-start note for wiring without a reset line: a daemon restart is a
 * warm start against whatever state the chip is in; recovery from a wedged
 * chip needs a power cycle. */
static void hw_log_reset_note(const char *band)
{
    if (daemon_hw_profile.reset_wired)
        return;

    printf("[%s] Hinweis: RESET nicht verdrahtet (Profil %s) – Warmstart, "
           "vorheriger Chip-Zustand möglich; Recovery nur per Power-Cycle\n",
           band, daemon_hw_profile.name);
}

/* Boot RF defaults live in the band descriptor (daemon_band.cpp). */

/* --- Radio startup/init -------------------------------------------------- */
void lora_init(void) {
    const DaemonBandDescriptor *band = daemon_band();
    const char *tag = band->tag;

    printf("[Init] Starte LoRa Receiver: radio=%s\n", tag);
    daemon_debug_ctx("RADIO", "Funk-Init beginnt");

    radio_controller.health = RADIO_HEALTH_UNINITIALIZED;
    daemon_debug_ctx("RADIO", "Health zurückgesetzt");

    /* Cross-process GPIO ownership (audit P1-1): claim every pin this
     * process will drive BEFORE any lgpio claim. lgpio claim errors are
     * printed-and-swallowed inside RadioLib's HAL, so this is the only
     * deterministic conflict gate between the two band processes. */
    {
        int pins[DAEMON_HW_MAX_CLAIMED + 1];
        size_t n = 0;

        for (int i = 0; i < daemon_hw_profile.claimed_count; i++)
            pins[n++] = daemon_hw_profile.claimed[i];
        pins[n++] = daemon_led_pin_configured(); /* may be overridden */

        if (!daemon_gpio_locks_acquire(pins, n)) {
            printf("[GPIO] Fehler: GPIO-Konflikt/Sperre – Radio-Init "
                   "abgebrochen (fail-closed)\n");
            radio_controller.health = RADIO_HEALTH_FAILED;
            return;
        }
    }

    if (!daemon_led_ready()) {
        printf("[GPIO] Fehler: LED/GPIO nicht bereit!\n");
        daemon_debug_ctx("GPIO", "Nicht bereit");
        radio_controller.health = RADIO_HEALTH_FAILED;
        return;
    }

    daemon_radio_runtime_led(&radio_controller, 1);

    daemon_debug_band(tag, "Objekte anlegen");
    hw_log_reset_note(tag);
    radio_controller.hal.reset(new LockingPiHal(0));
    radio_controller.mod.reset(new Module(
        radio_controller.hal.get(),
        hw_pin_or_nc(daemon_hw_profile.cs),
        hw_pin_or_nc(daemon_hw_profile.irq),
        hw_pin_or_nc(daemon_hw_profile.rst),
        hw_pin_or_nc(daemon_hw_profile.gpio)));
    radio_controller.driver.reset(
        hw_driver_create(radio_controller.mod.get(), band->is_hf));

    daemon_debug_band(tag, "begin()");
    /* Fail closed: never call begin() (which drives SPI) unless the
     * process-shared SPI lock was established. */
    int state;
    if (static_cast<LockingPiHal *>(radio_controller.hal.get())
            ->spi_lock_ready()) {
        state = radio_controller.driver->begin(band->rf_defaults);
    } else {
        state = RADIOLIB_ERR_SPI_CMD_FAILED;
        printf("[SPI] Fehler: SPI-Sperre für %s nicht verfügbar – "
               "begin() übersprungen\n", tag);
    }
    if (state == RADIOLIB_ERR_NONE) {
        radio_controller.health = RADIO_HEALTH_READY;
        printf("[%s] Init OK\n", tag);
        daemon_debug_ctx(tag, "Radio bereit");

        daemon_debug_band(tag, "LoRa-Default gesetzt");
        radio_controller.driver->setPacketReceivedAction(setFlag); // Callback nutzen
        daemon_debug_band(tag, "Callback gesetzt");
    } else {
        radio_controller.health = RADIO_HEALTH_FAILED;
        printf("[%s] Init FEHLGESCHLAGEN: %d\n", tag, state);
        hw_diagnose_begin_failure(radio_controller.mod.get(), tag, state);
        daemon_debug_band(tag, "begin() Fehler %d", state);
    }

    daemon_radio_runtime_led(&radio_controller, 0);

    if (radio_controller_ready(&radio_controller)) {
        daemon_debug_band(tag, "RX starten");
        daemon_rx_rearm_boot_result(&radio_controller,
                                    radio_controller.driver->startReceive());
    } else {
        printf("[%s] RX nicht gestartet: %s\n",
               tag, radio_health_name(radio_controller.health));
        daemon_debug_band(tag, "RX Start übersprungen");
    }

    daemon_debug_ctx("RADIO", "Funk-Init abgeschlossen");
    fflush(stdout);
}
