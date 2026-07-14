#include "daemon_radio_init.h"

#include <stdio.h>

#include "hal/RPi/PiHal.h"
#include <RadioLib.h>

#include "locking_pihal.h"

#include "daemon_led.h"
#include "daemon_log.h"
#include "daemon_radio_runtime.h"
#include "daemon_radio_selection.h"
#include "hardware_profile.h"
#include "radio_controller.h"
#include "radio_driver.h"
#include "radio_health.h"
#include "sx127x_driver.h"

/* --- Profile helpers ------------------------------------------------------ */

static uint32_t hw_pin_or_nc(int pin)
{
    return pin < 0 ? RADIOLIB_NC : (uint32_t)pin;
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

/* --- Boot-RF-Defaults ------------------------------------------------------ */
// Per-band boot defaults, applied inside RadioDriver::begin() in the exact
// pre-driver setter order. LDRO: 433 = autoLDRO()+forceLDRO(1), 868 = nur
// autoLDRO() (Feld ldro: >=0 forciert, <0 auto).

// LoRa-APRS:
static const RadioRfDefaults rf_defaults_433 = {
    433.900f,   /* freq_mhz */
    12,         /* spreading_factor */
    125.0f,     /* bandwidth_khz */
    0x12,       /* sync_word */
    8,          /* preamble_len */
    5,          /* coding_rate */
    true,       /* crc_on */
    1,          /* ldro: autoLDRO() + forceLDRO(1) */
    10          /* power_dbm */
};

/*
*
* // LoRa DX Cluster:
*    433.900 / SF10 / BW125.0 / Sync 0x12 / Preamble 8 / CR5 / CRC an /
*    autoLDRO() / 10 dBm
*
*
*
* // Meshtastic 433:
*
*    433.900 (DB0ARD) / SF11 / BW125.0 / Sync 0x2B / Preamble 16 / CR5 /
*    CRC an / autoLDRO() / 10 dBm
*
*
* // Meshcom:
*
*    433.175 / SF11 / BW250.0 / Sync 0x2B / Preamble 8 / CR6 / CRC an /
*    (kein autoLDRO) / 10 dBm
*/

static const RadioRfDefaults rf_defaults_868 = {
    869.525f,   /* freq_mhz */
    11,         /* spreading_factor */
    250.0f,     /* bandwidth_khz */
    0x2B,       /* sync_word */
    16,         /* preamble_len */
    5,          /* coding_rate */
    true,       /* crc_on */
    -1,         /* ldro: nur autoLDRO() */
    10          /* power_dbm */
};

/* --- Radio startup/init -------------------------------------------------- */
void lora_init(void) {
    printf("[Init] Starte LoRa Receiver: radio=%s\n",
           daemon_radio_selection_name(daemon_radio_selection));
    daemon_debug_ctx("RADIO", "Funk-Init beginnt");

    radio_controller_433.health = RADIO_HEALTH_UNINITIALIZED;
    radio_controller_868.health = RADIO_HEALTH_UNINITIALIZED;
    daemon_debug_ctx("RADIO", "Health zurückgesetzt");

    if (!daemon_led_ready()) {
        printf("[GPIO] Fehler: LED/GPIO nicht bereit!\n");
        daemon_debug_ctx("GPIO", "Nicht bereit");
        if (daemon_radio_433_enabled())
            radio_controller_433.health = RADIO_HEALTH_FAILED;
        if (daemon_radio_868_enabled())
            radio_controller_868.health = RADIO_HEALTH_FAILED;
        return;
    }

    if (daemon_radio_433_enabled() &&
        daemon_hw_profile.family != DAEMON_CHIP_FAMILY_SX127X) {
        radio_controller_433.health = RADIO_HEALTH_FAILED;
        printf("[433] Init FEHLGESCHLAGEN: Chip-Familie %s (Profil %s) wird "
               "von diesem Build noch nicht unterstützt\n",
               daemon_chip_family_name(daemon_hw_profile.family),
               daemon_hw_profile.name);
    } else if (daemon_radio_433_enabled()) {
        daemon_radio_runtime_led(&radio_controller_433, 1);

        daemon_debug_band("433", "Objekte anlegen");
        hw_log_reset_note("433");
        radio_controller_433.hal.reset(new LockingPiHal(0));
        radio_controller_433.mod.reset(new Module(
            radio_controller_433.hal.get(),
            hw_pin_or_nc(daemon_hw_profile.cs),
            hw_pin_or_nc(daemon_hw_profile.irq),
            hw_pin_or_nc(daemon_hw_profile.rst),
            hw_pin_or_nc(daemon_hw_profile.gpio)));
        radio_controller_433.driver.reset(
            sx127x_driver_create(radio_controller_433.mod.get(), false));

        daemon_debug_band("433", "begin()");
        /* Fail closed: never call begin() (which drives SPI) unless the
         * process-shared SPI lock was established. */
        int state_433;
        if (static_cast<LockingPiHal *>(radio_controller_433.hal.get())
                ->spi_lock_ready()) {
            state_433 = radio_controller_433.driver->begin(&rf_defaults_433);
        } else {
            state_433 = RADIOLIB_ERR_SPI_CMD_FAILED;
            printf("[SPI] Fehler: SPI-Sperre für 433 nicht verfügbar – "
                   "begin() übersprungen\n");
        }
        if (state_433 == RADIOLIB_ERR_NONE) {
            radio_controller_433.health = RADIO_HEALTH_READY;
            printf("[433] Init OK\n");
            daemon_debug_ctx("433", "Radio bereit");

            daemon_debug_band("433", "LoRa-Default gesetzt");
            radio_controller_433.driver->setPacketReceivedAction(setFlag433); // Callback nutzen
            daemon_debug_band("433", "Callback gesetzt");
        } else {
            radio_controller_433.health = RADIO_HEALTH_FAILED;
            printf("[433] Init FEHLGESCHLAGEN: %d\n", state_433);
            sx127x_diagnose_begin_failure(radio_controller_433.mod.get(),
                                          "433", state_433);
            daemon_debug_band("433", "begin() Fehler %d", state_433);
        }

        daemon_radio_runtime_led(&radio_controller_433, 0);
    } else {
        daemon_debug_band("433", "Nicht ausgewählt");
    }

    if (daemon_radio_868_enabled() &&
        daemon_hw_profile.family != DAEMON_CHIP_FAMILY_SX127X) {
        radio_controller_868.health = RADIO_HEALTH_FAILED;
        printf("[868] Init FEHLGESCHLAGEN: Chip-Familie %s (Profil %s) wird "
               "von diesem Build noch nicht unterstützt\n",
               daemon_chip_family_name(daemon_hw_profile.family),
               daemon_hw_profile.name);
    } else if (daemon_radio_868_enabled()) {
        daemon_radio_runtime_led(&radio_controller_868, 1);

        daemon_debug_band("868", "Objekte anlegen");
        hw_log_reset_note("868");
        radio_controller_868.hal.reset(new LockingPiHal(0));
        radio_controller_868.mod.reset(new Module(
            radio_controller_868.hal.get(),
            hw_pin_or_nc(daemon_hw_profile.cs),
            hw_pin_or_nc(daemon_hw_profile.irq),
            hw_pin_or_nc(daemon_hw_profile.rst),
            hw_pin_or_nc(daemon_hw_profile.gpio)));
        radio_controller_868.driver.reset(
            sx127x_driver_create(radio_controller_868.mod.get(), true));

        daemon_debug_band("868", "begin()");
        /* Fail closed: never call begin() (which drives SPI) unless the
         * process-shared SPI lock was established. */
        int state_868;
        if (static_cast<LockingPiHal *>(radio_controller_868.hal.get())
                ->spi_lock_ready()) {
            state_868 = radio_controller_868.driver->begin(&rf_defaults_868);
        } else {
            state_868 = RADIOLIB_ERR_SPI_CMD_FAILED;
            printf("[SPI] Fehler: SPI-Sperre für 868 nicht verfügbar – "
                   "begin() übersprungen\n");
        }
        if (state_868 == RADIOLIB_ERR_NONE) {
        radio_controller_868.health = RADIO_HEALTH_READY;
        printf("[868] Init OK\n");
        daemon_debug_ctx("868", "Radio bereit");

        daemon_debug_band("868", "LoRa-Default gesetzt");
        radio_controller_868.driver->setPacketReceivedAction(setFlag868); // Callback nutzen
        daemon_debug_band("868", "Callback gesetzt");
        } else {
            radio_controller_868.health = RADIO_HEALTH_FAILED;
            printf("[868] Init FEHLGESCHLAGEN: %d\n", state_868);
            sx127x_diagnose_begin_failure(radio_controller_868.mod.get(),
                                          "868", state_868);
            daemon_debug_band("868", "begin() Fehler %d", state_868);
        }

        daemon_radio_runtime_led(&radio_controller_868, 0);
    } else {
        daemon_debug_band("868", "Nicht ausgewählt");
    }

    if (daemon_radio_433_enabled() && radio_controller_ready(&radio_controller_433)) {
        daemon_debug_band("433", "RX starten");
        radio_controller_433.driver->startReceive();
    } else if (daemon_radio_433_enabled()) {
        printf("[433] RX nicht gestartet: %s\n",
               radio_health_name(radio_controller_433.health));
        daemon_debug_band("433", "RX Start übersprungen");
    } else {
        daemon_debug_band("433", "RX Start nicht ausgewählt");
    }

    if (daemon_radio_868_enabled() && radio_controller_ready(&radio_controller_868)) {
        daemon_debug_band("868", "RX starten");
        radio_controller_868.driver->startReceive();
    } else if (daemon_radio_868_enabled()) {
        printf("[868] RX nicht gestartet: %s\n",
               radio_health_name(radio_controller_868.health));
        daemon_debug_band("868", "RX Start übersprungen");
    } else {
        daemon_debug_band("868", "RX Start nicht ausgewählt");
    }

    daemon_debug_ctx("RADIO", "Funk-Init abgeschlossen");
    fflush(stdout);
}
