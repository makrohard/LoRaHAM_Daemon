#ifndef LORAHAM_SX1262_DRIVER_H
#define LORAHAM_SX1262_DRIVER_H

#include <memory>

#include <RadioLib.h>

#include "radio_driver.h"

/* --- SX1262-Treiber -------------------------------------------------------- */
/*
 * Konkreter RadioDriver für die SX126x-Familie (Waveshare SX1262 LoRaWAN
 * Node HAT, LF- und HF-Variante pin-identisch; das Band kommt aus --radio).
 *
 * Chip-Besonderheiten gegenüber SX127x, hier gekapselt:
 *  - TCXO wird aus DIO3 gespeist: begin()/beginFSK() setzen die
 *    TCXO-Referenzspannung aus dem Hardware-Profil.
 *  - RX/TX-Umschaltung: DIO2 als RF-Switch (setDio2AsRfSwitch), TXEN/ANT_SW
 *    optional als GPIO über Module::setRfSwitchPins.
 *  - Sync-Word: RadioLib bildet das SX127x-Byte (0x12/0x2B) über die
 *    Kompatibilitäts-Steuerbits ab (setSyncWord(byte, 0x44)); die
 *    On-Air-Kompatibilität zu SX127x-Gegenstellen ist Bench-Punkt der
 *    Hardware-Session.
 *  - CRC: setCRC(len) — LoRa-CRC an == 2 Byte, aus == 0.
 *  - Leistungsbereich −9…+22 dBm (RadioLib validiert; CONF-Policy bleibt
 *    0…20 und damit innerhalb des Chip-Bereichs).
 *  - Kein OOK; FSK-RXBW-Rasterwerte weichen vom SX127x ab (RadioLib
 *    validiert den chip-eigenen Raster und lehnt fremde Werte ab).
 *  - Live-RSSI über das GetRssiInst-Kommando (RadioLib getRSSI(false)),
 *    niemals über SX127x-Registeradressen.
 */

class Sx1262Driver : public RadioDriver {
public:
    Sx1262Driver(Module *mod, float tcxo_voltage, int txen_pin);

    int16_t begin(const RadioRfDefaults *defaults) override;
    int16_t switchMode(RadioMode_t mode,
                       const RadioRfDefaults *defaults) override;
    int16_t applyLoraParam(const char *tag, const std::string &key,
                        const std::string &val) override;
    int16_t applyFskParam(const char *tag, const std::string &key,
                       const std::string &val) override;
    float readLiveRssi(RadioMode_t mode, bool is_hf) override;
    float rssiProbe() override;
    const char *chipName() const override;
    DaemonChipFamily chipFamily() const override
    {
        return DAEMON_CHIP_FAMILY_SX1262;
    }

private:
    Module *mod_;
    std::unique_ptr<SX1262> radio_;
    float tcxo_voltage_;
    int txen_pin_;
};

RadioDriver *sx1262_driver_create(Module *mod, float tcxo_voltage,
                                  int txen_pin);

/*
 * D8 (SX126x): genau eine profilbewusste Diagnosezeile für ein
 * fehlgeschlagenes begin(). Verlässt sich auf die RadioLib-eigene
 * Chip-Verifikation (Versionsstring + BUSY-Protokoll); keine
 * handgerollten Pre-begin-Kommandos.
 */
void sx1262_diagnose_begin_failure(const char *band, int state);

#endif
