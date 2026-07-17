#include "config_apply.h"

#include "daemon_band.h"

#include <vector>
#include <utility>

#include <RadioLib.h>

#include "config_parser.h"
#include "config_validate.h"
#include "config_value.h"

/* --- CONFIG command apply ------------------------------------------------ */
// Chip-agnostic apply pipeline: parse, validate, MODE via
// RadioDriver::switchMode(), GETRSSI, then per-key parameters via
// RadioDriver::applyLoraParam()/applyFskParam(). The colored per-key state
// output for the chip parameters comes from the driver.

ConfigApplyStatus parse_and_apply_config_generic(RadioDriver &radio,
                                                 const char *tag,
                                                 const char *cmd,
                                                 RadioMode_t &mode_flag,
                                                 std::atomic<bool> &getrssi_flag) {
    ConfigCommand parsed = config_parse_command(cmd);

    if(!parsed.is_set) {
        printf("[%s] Unbekannter Befehl: %s\n", tag, parsed.text.c_str());
        return CONFIG_APPLY_NOOP;
    }

    if(!parsed.has_params)
        return CONFIG_APPLY_NOOP;

    ConfigValidationResult validation;
    if(!config_validate_command(parsed, mode_flag, &validation,
                                radio.chipFamily(),
                                daemon_band()->freq_min_mhz,
                                daemon_band()->freq_max_mhz)) {
        printf("[%s] CONFIG rejected: %s=%s (%s)\n",
               tag,
               validation.key.c_str(),
               validation.value.c_str(),
               validation.reason.c_str());
        return CONFIG_APPLY_REJECTED;
    }

    bool hardware_touched = false;

    bool printed = false;

    // First pass: collect MODE, GETRSSI and radio parameters.
    std::vector<std::pair<std::string,std::string>> tokens;
    std::vector<std::pair<std::string,std::string>> getrssi_tokens;
    std::string mode_val = parsed.mode;

    for(auto &kv : parsed.tokens) {
        const std::string &key = kv.first;

        if(key == "GETRSSI")
            getrssi_tokens.push_back(kv);
        else
            tokens.push_back(kv);
    }

    // Apply MODE before any mode-specific parameter.
    if(!mode_val.empty()) {
        config_apply_print_prefix(tag, &printed);

        if(mode_val == "FSK") {
            printf(" MODE=FSK -> beginFSK()");
            int state = radio.switchMode(RADIO_MODE_FSK,
                                         daemon_band()->rf_defaults);
            if(state == RADIOLIB_ERR_NONE) {
                mode_flag = RADIO_MODE_FSK;
                hardware_touched = true;
                printf(" \033[92mOK\033[0m");
            } else {
                printf(" \033[91;5mFEHLER:%d\033[0m ABORT\n", state);

                return CONFIG_APPLY_HW_ERROR;
            }
        } else if(mode_val == "LORA") {
            printf(" MODE=LORA -> begin()");
            int state = radio.switchMode(RADIO_MODE_LORA,
                                         daemon_band()->rf_defaults);
            if(state == RADIOLIB_ERR_NONE) {
                mode_flag = RADIO_MODE_LORA;
                hardware_touched = true;
                printf(" \033[92mOK\033[0m");
            } else {
                printf(" \033[91;5mFEHLER:%d\033[0m ABORT\n", state);

                return CONFIG_APPLY_HW_ERROR;
            }
        } else {
            printf(" MODE=\033[91;5m%s\033[0m (unbekannt, ignoriert)", mode_val.c_str());
        }
    }

    // Apply GETRSSI after a successful MODE switch.
    for(auto &kv : getrssi_tokens) {
        const std::string &val = kv.second;
        int v = 0;

        config_apply_print_prefix(tag, &printed);
        if(config_value_parse_bool01_exact(val, &v)) {
            getrssi_flag.store(v != 0);
            if(v == 1) printf(" GETRSSI=\033[92m1\033[0m");
            else       printf(" GETRSSI=\033[92m0\033[0m");
        } else {
            printf(" GETRSSI=\033[91;5m%s\033[0m", val.c_str());
        }
    }

    // Second pass: apply remaining parameters for the active mode.
    for(auto &kv : tokens) {
        const std::string &key = kv.first;
        const std::string &val = kv.second;

        if(mode_flag == RADIO_MODE_FSK) {
            // Ignore LoRa-only keys while in FSK mode.
            if(key=="SF" || key=="BW" || key=="CR" || key=="LDRO" || key=="CRC") {
                printf(" \033[93m%s=IGNORIERT(LoRa-Key im FSK-Modus)\033[0m", key.c_str());
            } else {
                int16_t state = radio.applyFskParam(tag, key, val);
                if (state != RADIOLIB_ERR_NONE) {
                    printf(" \033[91;5mABORT nach %s (Fehler %d)\033[0m\n",
                           key.c_str(), (int)state);
                    return CONFIG_APPLY_HW_ERROR;
                }
                hardware_touched = true;
            }
        } else {
            // Ignore FSK-only keys while in LoRa mode.
            if(key=="BR" || key=="FREQDEV" || key=="RXBW" || key=="OOK" ||
                key=="SHAPING" || key=="ENCODING") {
                printf(" \033[93m%s=IGNORIERT(FSK-Key im LoRa-Modus, SET MODE=FSK fehlt)\033[0m", key.c_str());
                } else {
                    int16_t state = radio.applyLoraParam(tag, key, val);
                    if (state != RADIOLIB_ERR_NONE) {
                        printf(" \033[91;5mABORT nach %s (Fehler %d)\033[0m\n",
                               key.c_str(), (int)state);
                        return CONFIG_APPLY_HW_ERROR;
                    }
                    hardware_touched = true;
                }
        }
    }
    printf("\n");

    return hardware_touched ? CONFIG_APPLY_APPLIED : CONFIG_APPLY_NOOP;
}
