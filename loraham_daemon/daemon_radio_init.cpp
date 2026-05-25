#include "daemon_radio_init.h"

#include <stdio.h>

#include "hal/RPi/PiHal.h"
#include <RadioLib.h>

#include "daemon_led.h"
#include "daemon_log.h"
#include "daemon_radio_runtime.h"
#include "daemon_radio_selection.h"
#include "radio_controller.h"
#include "radio_health.h"

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

    if (daemon_radio_433_enabled()) {
        daemon_radio_runtime_led(&radio_controller_433, 1);

        daemon_debug_band("433", "Objekte anlegen");
        radio_controller_433.hal.reset(new PiHal(0));
        radio_controller_433.mod.reset(new Module(radio_controller_433.hal.get(), 8, 25, 5, 24));
        radio_controller_433.radio.reset(new SX1278(radio_controller_433.mod.get()));

        daemon_debug_band("433", "begin()");
        int state_433 = radio_controller_433.radio->begin();
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
            daemon_debug_band("433", "begin() Fehler %d", state_433);
        }

        daemon_radio_runtime_led(&radio_controller_433, 0);
    } else {
        daemon_debug_band("433", "Nicht ausgewählt");
    }

    if (daemon_radio_868_enabled()) {
        daemon_radio_runtime_led(&radio_controller_868, 1);

        daemon_debug_band("868", "Objekte anlegen");
        radio_controller_868.hal.reset(new PiHal(0));
        radio_controller_868.mod.reset(new Module(radio_controller_868.hal.get(), 7, 16, 6, 12));
        radio_controller_868.radio.reset(new RFM95(radio_controller_868.mod.get()));

        daemon_debug_band("868", "begin()");
        int state_868 = radio_controller_868.radio->begin();
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
