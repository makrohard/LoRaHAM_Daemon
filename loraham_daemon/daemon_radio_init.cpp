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
#include "radio_health.h"

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

/*
 * D8: exactly one profile-aware diagnosis line for a failed SX127x begin().
 * For CHIP_NOT_FOUND a raw RegVersion (0x42) read distinguishes "no response"
 * (module absent / CE switch / wrong profile) from "responds with unexpected
 * ID" (wrong chip family / wrong profile). Log-only heuristic; the
 * authoritative gate stays begin()'s return and the fail-closed health path.
 * The read goes through Module::SPIgetRegValue and thus inherits the SPI
 * flock and runs only after begin() already failed.
 */
static void hw_diagnose_sx127x_failure(Module *mod, const char *band,
                                       int state)
{
    const DaemonHardwareProfile *hw = &daemon_hw_profile;
    char pins[96];

    snprintf(pins, sizeof(pins), "CS=%d DIO0=%d RST=%d DIO1=%d LED=%d",
             hw->cs, hw->irq, hw->rst, hw->gpio, hw->led_pin);

    if (state == RADIOLIB_ERR_CHIP_NOT_FOUND && mod) {
        int16_t ver = mod->SPIgetRegValue(0x42, 7, 0);

        if (ver <= 0 || ver == 0x00 || ver == 0xFF) {
            printf("[%s] Diagnose (Profil %s): keine Antwort auf CS=BCM%d – "
                   "Modul fehlt, CE-Schalter falsch oder falsches Profil; "
                   "Pins laut Profil: %s (hält ein anderer Prozess eine "
                   "dieser Leitungen?)\n",
                   band, hw->name, hw->cs, pins);
        } else {
            printf("[%s] Diagnose (Profil %s): Chip antwortet mit ID 0x%02X "
                   "(erwartet 0x12) – falsche Chip-Familie oder falsches "
                   "Profil; Pins laut Profil: %s\n",
                   band, hw->name, (unsigned)ver, pins);
        }
        return;
    }

    printf("[%s] Diagnose (Profil %s): begin() Fehler %d; Pins laut Profil: "
           "%s (hält ein anderer Prozess eine dieser Leitungen?)\n",
           band, hw->name, state, pins);
}

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
        radio_controller_433.radio.reset(new SX1278(radio_controller_433.mod.get()));

        daemon_debug_band("433", "begin()");
        /* Fail closed: never call begin() (which drives SPI) unless the
         * process-shared SPI lock was established. */
        int state_433;
        if (static_cast<LockingPiHal *>(radio_controller_433.hal.get())
                ->spi_lock_ready()) {
            state_433 = radio_controller_433.radio->begin();
        } else {
            state_433 = RADIOLIB_ERR_SPI_CMD_FAILED;
            printf("[SPI] Fehler: SPI-Sperre für 433 nicht verfügbar – "
                   "begin() übersprungen\n");
        }
        if (state_433 == RADIOLIB_ERR_NONE) {
            radio_controller_433.health = RADIO_HEALTH_READY;
            printf("[433] Init OK\n");
            daemon_debug_ctx("433", "Radio bereit");

            // LoRa-APRS:
            radio_controller_433.radio->setFrequency(433.900);
            radio_controller_433.radio->setSpreadingFactor(12);
            radio_controller_433.radio->setBandwidth(125.0);
            radio_controller_433.radio->setSyncWord(0x12);
            radio_controller_433.radio->setPreambleLength(8);
            radio_controller_433.radio->setCodingRate(5);
            radio_controller_433.radio->setCRC(true);
            radio_controller_433.radio->autoLDRO();
            radio_controller_433.radio->forceLDRO(1);
            radio_controller_433.radio->setOutputPower(10);

            /*
            *
            * // LoRa DX Cluster:
            *    radio_controller_433.radio->setFrequency(433.900);
            *    radio_controller_433.radio->setSpreadingFactor(10);
            *    radio_controller_433.radio->setBandwidth(125.0);
            *    radio_controller_433.radio->setSyncWord(0x12);
            *    radio_controller_433.radio->setPreambleLength(8);
            *    radio_controller_433.radio->setCodingRate(5);
            *    radio_controller_433.radio->setCRC(true);
            *    radio_controller_433.radio->autoLDRO();
            *    radio_controller_433.radio->setOutputPower(10);
            *
            *
            *
            * // Meshtastic 433:
            *
            *    radio_controller_433.radio->setFrequency(433.900); // DB0ARD
            *    radio_controller_433.radio->setSpreadingFactor(11);
            *    radio_controller_433.radio->setBandwidth(125.0);
            *    radio_controller_433.radio->setSyncWord(0x2B);
            *    radio_controller_433.radio->setPreambleLength(16);
            *    radio_controller_433.radio->setCodingRate(5);
            *    radio_controller_433.radio->setCRC(true);
            *    radio_controller_433.radio->autoLDRO();
            *    radio_controller_433.radio->setOutputPower(10);
            *
            *
            * // Meshcom:
            *
            *    radio_controller_433.radio->setFrequency(433.175);
            *    radio_controller_433.radio->setSpreadingFactor(11);
            *    radio_controller_433.radio->setBandwidth(250.0);
            *    radio_controller_433.radio->setSyncWord(0x2B);
            *    radio_controller_433.radio->setPreambleLength(8);
            *    radio_controller_433.radio->setCodingRate(6);
            *    radio_controller_433.radio->setCRC(true);
            *    //radio_controller_433.radio->autoLDRO();
            *    radio_controller_433.radio->setOutputPower(10);
            */
            daemon_debug_band("433", "LoRa-Default gesetzt");
            radio_controller_433.radio->setPacketReceivedAction(setFlag433); // Callback nutzen
            daemon_debug_band("433", "Callback gesetzt");
        } else {
            radio_controller_433.health = RADIO_HEALTH_FAILED;
            printf("[433] Init FEHLGESCHLAGEN: %d\n", state_433);
            hw_diagnose_sx127x_failure(radio_controller_433.mod.get(),
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
        radio_controller_868.radio.reset(new RFM95(radio_controller_868.mod.get()));

        daemon_debug_band("868", "begin()");
        /* Fail closed: never call begin() (which drives SPI) unless the
         * process-shared SPI lock was established. */
        int state_868;
        if (static_cast<LockingPiHal *>(radio_controller_868.hal.get())
                ->spi_lock_ready()) {
            state_868 = radio_controller_868.radio->begin();
        } else {
            state_868 = RADIOLIB_ERR_SPI_CMD_FAILED;
            printf("[SPI] Fehler: SPI-Sperre für 868 nicht verfügbar – "
                   "begin() übersprungen\n");
        }
        if (state_868 == RADIOLIB_ERR_NONE) {
        radio_controller_868.health = RADIO_HEALTH_READY;
        printf("[868] Init OK\n");
        daemon_debug_ctx("868", "Radio bereit");

        radio_controller_868.radio->setFrequency(869.525);
        radio_controller_868.radio->setSpreadingFactor(11);
        radio_controller_868.radio->setBandwidth(250.0);
        radio_controller_868.radio->setSyncWord(0x2B);
        radio_controller_868.radio->setPreambleLength(16);
        radio_controller_868.radio->setCodingRate(5);
        radio_controller_868.radio->setCRC(true);
        radio_controller_868.radio->autoLDRO();
        radio_controller_868.radio->setOutputPower(10);

        daemon_debug_band("868", "LoRa-Default gesetzt");
        radio_controller_868.radio->setPacketReceivedAction(setFlag868); // Callback nutzen
        daemon_debug_band("868", "Callback gesetzt");
        } else {
            radio_controller_868.health = RADIO_HEALTH_FAILED;
            printf("[868] Init FEHLGESCHLAGEN: %d\n", state_868);
            hw_diagnose_sx127x_failure(radio_controller_868.mod.get(),
                                       "868", state_868);
            daemon_debug_band("868", "begin() Fehler %d", state_868);
        }

        daemon_radio_runtime_led(&radio_controller_868, 0);
    } else {
        daemon_debug_band("868", "Nicht ausgewählt");
    }

    if (daemon_radio_433_enabled() && radio_controller_ready(&radio_controller_433)) {
        daemon_debug_band("433", "RX starten");
        radio_controller_433.radio->startReceive();
    } else if (daemon_radio_433_enabled()) {
        printf("[433] RX nicht gestartet: %s\n",
               radio_health_name(radio_controller_433.health));
        daemon_debug_band("433", "RX Start übersprungen");
    } else {
        daemon_debug_band("433", "RX Start nicht ausgewählt");
    }

    if (daemon_radio_868_enabled() && radio_controller_ready(&radio_controller_868)) {
        daemon_debug_band("868", "RX starten");
        radio_controller_868.radio->startReceive();
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
