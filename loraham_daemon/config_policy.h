#ifndef LORAHAM_CONFIG_POLICY_H
#define LORAHAM_CONFIG_POLICY_H

#include <stdint.h>

#include "hardware_profile.h"   /* DaemonChipFamily */

/* --- CONFIG value policy --- */

bool config_policy_lora_sf_valid(int sf);
bool config_policy_lora_bandwidth_valid(float bw);
bool config_policy_lora_cr_valid(int cr);
bool config_policy_lora_preamble_valid(int preamble);
bool config_policy_lora_sync_valid(uint32_t sync);
bool config_policy_power_valid(int power);

bool config_policy_fsk_bitrate_valid(float br);
bool config_policy_fsk_freqdev_valid(float freqdev);
bool config_policy_fsk_rxbw_valid(float bw);
bool config_policy_fsk_rxbw_valid_sx126x(float bw);
bool config_policy_fsk_rxbw_valid_family(float bw, DaemonChipFamily family);
bool config_policy_fsk_preamble_valid(int preamble);
bool config_policy_fsk_sync_valid(uint32_t sync);

bool config_policy_freq_valid_band(float freq_mhz,
                                   float min_mhz,
                                   float max_mhz);

#endif
