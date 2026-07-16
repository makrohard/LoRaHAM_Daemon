#include "sx127x_driver.h"

#include <stdio.h>

#include "config_policy.h"
#include "driver_config_print.h"
#include "config_value.h"
#include "hardware_profile.h"

/* --- Konstruktion --------------------------------------------------------- */

Sx127xDriver::Sx127xDriver(Module *mod, bool is_hf)
    : RadioDriver(nullptr),
      mod_(mod),
      radio_(is_hf ? new RFM95(mod) : new SX1278(mod)),
      is_hf_(is_hf)
{
    phy_ = radio_.get();
}

RadioDriver *sx127x_driver_create(Module *mod, bool is_hf)
{
    return new Sx127xDriver(mod, is_hf);
}

const char *Sx127xDriver::chipName() const
{
    return is_hf_ ? "RFM95" : "SX1278";
}

/* --- Boot-Init mit RF-Defaults -------------------------------------------- */
// Reihenfolge der Setter exakt wie in der Vor-Treiber-Initialisierung
// (daemon_radio_init): Frequenz, SF, BW, Sync, Preamble, CR, CRC, LDRO,
// Leistung. LDRO <0 = nur autoLDRO(), >=0 = autoLDRO() + forceLDRO(Wert).

int16_t Sx127xDriver::begin(const RadioRfDefaults *defaults)
{
    int16_t state = radio_->begin();

    if (state != RADIOLIB_ERR_NONE || !defaults)
        return state;

    radio_->setFrequency(defaults->freq_mhz);
    radio_->setSpreadingFactor(defaults->spreading_factor);
    radio_->setBandwidth(defaults->bandwidth_khz);
    radio_->setSyncWord(defaults->sync_word);
    radio_->setPreambleLength(defaults->preamble_len);
    radio_->setCodingRate(defaults->coding_rate);
    radio_->setCRC(defaults->crc_on);
    /* LDRO immer explizit ins Register schreiben: autoLDRO() setzt nur das
     * Cache-Flag, und RadioLib schreibt nur bei Cache-Differenz. Ohne
     * RESET-Leitung (Uputronics) überlebt ein zuvor forciertes LDRO-Bit
     * sonst den Neustart im Chip, während der Cache vom POR-Zustand
     * ausgeht (bench-verifiziert: korrupte Dekodierung bei SF11/BW250). */
    bool ldro_needed = ((float)(1u << defaults->spreading_factor) /
                        defaults->bandwidth_khz) >= 16.0f;
    radio_->forceLDRO(defaults->ldro >= 0 ? (defaults->ldro != 0)
                                          : ldro_needed);
    if (defaults->ldro < 0)
        radio_->autoLDRO();
    radio_->setOutputPower(defaults->power_dbm);

    return state;
}

/* --- LoRa <-> FSK Modemwechsel -------------------------------------------- */

int16_t Sx127xDriver::switchMode(RadioMode_t mode)
{
    if (mode == RADIO_MODE_FSK)
        return radio_->beginFSK();

    int16_t state = radio_->begin();

    if (state == RADIOLIB_ERR_NONE) {
        /* begin() geht vom POR-Zustand aus und schreibt LDRO nur bei
         * Cache-Differenz; ohne RESET-Leitung bleibt ein altes forciertes
         * LDRO-Bit sonst im Chip stehen. Explizit auf den begin()-Zustand
         * (SF9/BW125 -> LDRO aus) zurückschreiben, danach Auto-Tracking
         * für nachfolgende SF/BW-Setter. */
        radio_->forceLDRO(false);
        radio_->autoLDRO();
    }

    return state;
}

/* --- Live-RSSI (roher Registerzugriff) ------------------------------------ */
// Read live RSSI directly from the SX127x register.

float Sx127xDriver::readLiveRssi(RadioMode_t mode, bool is_hf)
{
    uint8_t reg = (mode == RADIO_MODE_LORA) ? 0x1B : 0x11;
    int16_t raw = mod_->SPIgetRegValue(reg, 7, 0);

    if (raw < 0)
        return -200.0f;

    if (mode == RADIO_MODE_LORA)
        return (is_hf ? -157.0f : -164.0f) + (float)raw;

    return -((float)raw) / 2.0f;
}

/* --- Nicht-destruktive Sofort-RSSI-Probe ---------------------------------- */
// Live channel RSSI: packet=false reads the instant RSSI register (current
// channel energy, not the stale last-packet RSSI), skipReceive=true avoids
// re-entering RX. Non-destructive, same source as the GETRSSI live stream.

float Sx127xDriver::rssiProbe()
{
    return radio_->getRSSI(false, true);
}

/* --- D8-Diagnose für fehlgeschlagenes begin() ------------------------------ */
/*
 * The read goes through Module::SPIgetRegValue and thus inherits the SPI
 * flock and runs only after begin() already failed.
 */
void sx127x_diagnose_begin_failure(Module *mod, const char *band, int state)
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



/* --- LoRa CONFIG apply --------------------------------------------------- */

void Sx127xDriver::applyLoraParam(const char *tag,
                                  const std::string &key,
                                  const std::string &val)
{
    SX1278 &radio = *radio_;
    (void)tag;
    int state = 0;

    if (key == "SF") {
        int sf = 0;
        if (config_value_parse_int_exact(val, &sf) && config_policy_lora_sf_valid(sf)) {
            state = radio.setSpreadingFactor(sf);
            driver_config_print_state_int("SF", sf, state);
        } else {
            driver_config_print_rejected("SF", val);
        }
    }

    if (key == "BW") {
        float bw = 0.0f;
        if (config_value_parse_float_exact(val, &bw) && config_policy_lora_bandwidth_valid(bw)) {
            state = radio.setBandwidth(bw);
            driver_config_print_state_float("BW", bw, state);
        } else {
            driver_config_print_rejected("BW", val);
        }
    }

    if (key == "FREQ") {
        float f = 0.0f;
        if (config_value_parse_float_exact(val, &f) && f > 0.0f) {
            state = radio.setFrequency(f);
            if (state == RADIOLIB_ERR_NONE)
                printf(" FREQ=\033[92m%.6f\033[0m", f);
            else
                printf(" FREQ=\033[91;5m%.6f\033[0m", f);
        } else {
            driver_config_print_rejected("FREQ", val);
        }
    }

    if (key == "CR") {
        int cr = 0;
        if (config_value_parse_int_exact(val, &cr) && config_policy_lora_cr_valid(cr)) {
            state = radio.setCodingRate(cr);
            driver_config_print_state_int("CR", cr, state);
        } else {
            driver_config_print_rejected("CR", val);
        }
    }

    if (key == "CRC") {
        int crc = 0;
        if (config_value_parse_bool01_exact(val, &crc)) {
            state = radio.setCRC(crc != 0);
            driver_config_print_state_int("CRC", crc, state);
        } else {
            driver_config_print_rejected("CRC", val);
        }
    }

    if (key == "PREAMBLE") {
        int pre = 0;
        if (config_value_parse_int_exact(val, &pre) && config_policy_lora_preamble_valid(pre)) {
            state = radio.setPreambleLength(pre);
            driver_config_print_state_int("PREAMBLE", pre, state);
        } else {
            driver_config_print_rejected("PREAMBLE", val);
        }
    }

    if (key == "SYNC") {
        uint32_t sw = 0;
        if (config_value_parse_hex_or_dec_u32_exact(val, &sw) && config_policy_lora_sync_valid(sw)) {
            state = radio.setSyncWord((uint8_t)sw);
            if (state == RADIOLIB_ERR_NONE)
                printf(" SYNC=\033[92m0x%02X\033[0m", (unsigned)sw);
            else
                printf(" SYNC=\033[91;5m0x%02X\033[0m", (unsigned)sw);
        } else {
            driver_config_print_rejected("SYNC", val);
        }
    }

    if (key == "LDRO") {
        std::string norm = config_value_lower_ascii(config_value_trim_ascii(val));

        if (norm == "auto") {
            state = radio.autoLDRO();
            if (state == RADIOLIB_ERR_NONE)
                printf(" LDRO=\033[92mAUTO\033[0m");
            else
                printf(" LDRO=\033[91;5mAUTO\033[0m");
        } else {
            int ldro = 0;
            if (config_value_parse_bool01_exact(val, &ldro)) {
                state = radio.forceLDRO(ldro != 0);
                driver_config_print_state_int("LDRO", ldro, state);
            } else {
                driver_config_print_rejected("LDRO", val);
            }
        }
    }

    if (key == "POWER") {
        int p = 0;
        if (config_value_parse_int_exact(val, &p) && config_policy_power_valid(p)) {
            state = radio.setOutputPower(p);
            driver_config_print_state_int("POWER", p, state);
        } else {
            driver_config_print_rejected("POWER", val);
        }
    }

    fflush(stdout);
}

/* --- FSK CONFIG apply ---------------------------------------------------- */

void Sx127xDriver::applyFskParam(const char *tag,
                                 const std::string &key,
                                 const std::string &val)
{
    SX1278 &radio = *radio_;
    (void)tag;
    int state = 0;

    if (key == "FREQ") {
        float f = 0.0f;
        if (config_value_parse_float_exact(val, &f) && f > 0.0f) {
            state = radio.setFrequency(f);
            if (state == RADIOLIB_ERR_NONE)
                printf(" FREQ=\033[92m%.6f\033[0m", f);
            else
                printf(" FREQ=\033[91;5m%.6f\033[0m", f);
        } else {
            driver_config_print_rejected("FREQ", val);
        }
    }

    if (key == "POWER") {
        int p = 0;
        if (config_value_parse_int_exact(val, &p) && config_policy_power_valid(p)) {
            state = radio.setOutputPower(p);
            driver_config_print_state_int("POWER", p, state);
        } else {
            driver_config_print_rejected("POWER", val);
        }
    }

    if (key == "BR") {
        float br = 0.0f;
        if (config_value_parse_float_exact(val, &br) && config_policy_fsk_bitrate_valid(br)) {
            state = radio.setBitRate(br);
            driver_config_print_state_float("BR", br, state);
        } else {
            driver_config_print_rejected("BR", val);
        }
    }

    if (key == "FREQDEV") {
        float fd = 0.0f;
        if (config_value_parse_float_exact(val, &fd) && config_policy_fsk_freqdev_valid(fd)) {
            state = radio.setFrequencyDeviation(fd);
            driver_config_print_state_float("FREQDEV", fd, state);
        } else {
            driver_config_print_rejected("FREQDEV", val);
        }
    }

    if (key == "RXBW") {
        float bw = 0.0f;
        if (config_value_parse_float_exact(val, &bw) && config_policy_fsk_rxbw_valid(bw)) {
            state = radio.setRxBandwidth(bw);
            driver_config_print_state_float("RXBW", bw, state);
        } else {
            driver_config_print_rejected("RXBW", val);
        }
    }

    if (key == "OOK") {
        int ook = 0;
        if (config_value_parse_bool01_exact(val, &ook)) {
            state = radio.setOOK(ook != 0);
            driver_config_print_state_int("OOK", ook, state);
        } else {
            driver_config_print_rejected("OOK", val);
        }
    }

    if (key == "SHAPING") {
        uint8_t sh = 0;
        const char *sh_label = NULL;
        if (driver_parse_fsk_shaping(val, &sh, &sh_label)) {
            state = radio.setDataShaping(sh);
            if (state == RADIOLIB_ERR_NONE)
                printf(" SHAPING=\033[92m%s\033[0m", sh_label);
            else
                printf(" SHAPING=\033[91;5m%s\033[0m", sh_label);
        } else {
            driver_config_print_rejected("SHAPING", val);
        }
    }

    if (key == "ENCODING") {
        int enc = 0;
        if (config_value_parse_int_exact(val, &enc) && enc >= 0 && enc <= 2) {
            state = radio.setEncoding((uint8_t)enc);
            driver_config_print_state_int("ENCODING", enc, state);
        } else {
            driver_config_print_rejected("ENCODING", val);
        }
    }

    if (key == "PREAMBLE") {
        int pre = 0;
        if (config_value_parse_int_exact(val, &pre) && config_policy_fsk_preamble_valid(pre)) {
            state = radio.setPreambleLength(pre);
            driver_config_print_state_int("PREAMBLE", pre, state);
        } else {
            driver_config_print_rejected("PREAMBLE", val);
        }
    }

    if (key == "SYNC") {
        uint32_t raw = 0;
        if (config_value_parse_hex_or_dec_u32_exact(val, &raw) && config_policy_fsk_sync_valid(raw)) {
            if (raw <= 0xFF) {
                uint8_t sw[1] = { (uint8_t)raw };
                state = radio.setSyncWord(sw, 1);
                if (state == RADIOLIB_ERR_NONE)
                    printf(" SYNC=\033[92m0x%02X\033[0m", (unsigned)sw[0]);
                else
                    printf(" SYNC=\033[91;5m0x%02X\033[0m", (unsigned)sw[0]);
            } else {
                uint8_t sw[2] = { (uint8_t)(raw >> 8), (uint8_t)(raw & 0xFF) };
                state = radio.setSyncWord(sw, 2);
                if (state == RADIOLIB_ERR_NONE)
                    printf(" SYNC=\033[92m0x%04X\033[0m", (unsigned)raw);
                else
                    printf(" SYNC=\033[91;5m0x%04X\033[0m", (unsigned)raw);
            }
        } else {
            driver_config_print_rejected("SYNC", val);
        }
    }

    fflush(stdout);
}
