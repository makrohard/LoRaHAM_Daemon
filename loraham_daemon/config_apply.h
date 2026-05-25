#ifndef LORAHAM_CONFIG_APPLY_H
#define LORAHAM_CONFIG_APPLY_H

#include "radio_channel.h"
#include <vector>
#include <utility>
#include <string>
#include <stdlib.h>
#include <atomic>
#include <stdio.h>
#include <RadioLib.h>
#include "config_parser.h"
#include "config_value.h"
#include "config_validate.h"

/* --- CONFIG apply callback --- */

template<typename RadioT>
using ConfigApplyFn = void (*)(RadioT& radio,
                               const char *tag,
                               const char *cmd,
                               RadioMode_t& mode,
                               std::atomic<bool>& getrssi_active);

/* --- Low-level CONFIG parameter apply functions --- */

void apply_lora_param(SX1278 &radio, const char *tag,
                      const std::string &key, const std::string &val);
void apply_lora_param(RFM95 &radio, const char *tag,
                      const std::string &key, const std::string &val);
void apply_fsk_param(SX1278 &radio, const char *tag,
                     const std::string &key, const std::string &val);
void apply_fsk_param(RFM95 &radio, const char *tag,
                     const std::string &key, const std::string &val);

/* --- CONFIG command apply ------------------------------------------------ */
// Apply MODE first, then GETRSSI, then mode-specific radio parameters.
// SET without MODE keeps the current mode for backward compatibility.
static inline void config_apply_print_prefix(const char *tag, bool *printed)
{
    if (!*printed) {
        printf("[%s]", tag);
        *printed = true;
    }
}

template<typename RadioT>
void parse_and_apply_config_generic(RadioT &radio, const char *tag, const char *cmd,
                                    RadioMode_t &mode_flag,
                                    std::atomic<bool> &getrssi_flag) {
    ConfigCommand parsed = config_parse_command(cmd);

    if(!parsed.is_set) {
        printf("[%s] Unbekannter Befehl: %s\n", tag, parsed.text.c_str());
        return;
    }

    if(!parsed.has_params)
        return;

    ConfigValidationResult validation;
    if(!config_validate_command(parsed, mode_flag, &validation)) {
        printf("[%s] CONFIG rejected: %s=%s (%s)\n",
               tag,
               validation.key.c_str(),
               validation.value.c_str(),
               validation.reason.c_str());
        return;
    }

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
            int state = radio.beginFSK();
            if(state == RADIOLIB_ERR_NONE) {
                mode_flag = RADIO_MODE_FSK;
                printf(" \033[92mOK\033[0m");
            } else {
                printf(" \033[91;5mFEHLER:%d\033[0m ABORT\n", state);

                return;
            }
        } else if(mode_val == "LORA") {
            printf(" MODE=LORA -> begin()");
            int state = radio.begin();
            if(state == RADIOLIB_ERR_NONE) {
                mode_flag = RADIO_MODE_LORA;
                printf(" \033[92mOK\033[0m");
            } else {
                printf(" \033[91;5mFEHLER:%d\033[0m ABORT\n", state);

                return;
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
                apply_fsk_param(radio, tag, key, val);
            }
        } else {
            // Ignore FSK-only keys while in LoRa mode.
            if(key=="BR" || key=="FREQDEV" || key=="RXBW" || key=="OOK" ||
                key=="SHAPING" || key=="ENCODING") {
                printf(" \033[93m%s=IGNORIERT(FSK-Key im LoRa-Modus, SET MODE=FSK fehlt)\033[0m", key.c_str());
                } else {
                    apply_lora_param(radio, tag, key, val);
                }
        }
    }
    printf("\n");
}

template<typename RadioT>
static void config_apply_command(RadioT& radio,
                                 const char *tag,
                                 const char *cmd,
                                 RadioMode_t& mode,
                                 std::atomic<bool>& getrssi_active)
{
    parse_and_apply_config_generic<RadioT>(radio, tag, cmd, mode, getrssi_active);
}

#endif
