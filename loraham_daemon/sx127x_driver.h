#ifndef LORAHAM_SX127X_DRIVER_H
#define LORAHAM_SX127X_DRIVER_H

#include <memory>

#include <RadioLib.h>

#include "radio_driver.h"

/* --- SX127x-Treiberfamilie ------------------------------------------------ */
/*
 * Konkreter RadioDriver für die SX127x-Familie (SX1278 auf 433, RFM95 auf
 * 868). RFM95 ist in RadioLib ein SX1276-Alias und erbt von SX1278; das
 * Objekt wird als SX1278* gehalten, alle chip-abweichenden Methoden (begin,
 * beginFSK, setFrequency, ...) sind virtuell und dispatchen korrekt.
 *
 * Sämtliche SX127x-Registerkonstanten (RegRssiValue 0x1B, RegRssiValueFSK
 * 0x11, RegVersion 0x42) leben ausschließlich in sx127x_driver.cpp.
 */

class Sx127xDriver : public RadioDriver {
public:
    Sx127xDriver(Module *mod, bool is_hf);

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
        return DAEMON_CHIP_FAMILY_SX127X;
    }

private:
    Module *mod_;
    std::unique_ptr<SX1278> radio_;
    bool is_hf_;
};

/* Fabrik: is_hf=false -> SX1278 ("SX1278"), is_hf=true -> RFM95 ("RFM95"). */
RadioDriver *sx127x_driver_create(Module *mod, bool is_hf);

/*
 * D8: genau eine profilbewusste Diagnosezeile für ein fehlgeschlagenes
 * SX127x-begin(). Für CHIP_NOT_FOUND unterscheidet ein roher
 * RegVersion-Read (0x42) "keine Antwort" von "antwortet mit unerwarteter
 * ID". Nur Log-Heuristik; das autoritative Gate bleibt der begin()-Status
 * und der fail-closed Health-Pfad.
 */
void sx127x_diagnose_begin_failure(Module *mod, const char *band, int state);

#endif
