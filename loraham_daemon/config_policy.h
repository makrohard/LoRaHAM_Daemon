#ifndef LORAHAM_CONFIG_POLICY_H
#define LORAHAM_CONFIG_POLICY_H

#include <stddef.h>
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

bool config_policy_fsk_freqdev_valid_family(float freqdev,
                                            DaemonChipFamily family);
bool config_policy_fsk_ook_valid_family(int ook, DaemonChipFamily family);
bool config_policy_fsk_encoding_valid_family(int encoding,
                                             DaemonChipFamily family);

/* Airtime policy (audit P1-7): worst-case (255-byte payload, CRC on)
 * per-packet airtime of an accepted configuration must stay below this,
 * comfortably under the systemd stop timeout (30 s). */
#define CONFIG_POLICY_MAX_AIRTIME_MS 20000.0

double config_policy_lora_airtime_ms(int sf, float bw_khz, int cr,
                                     int preamble, size_t payload_len);
double config_policy_fsk_airtime_ms(float br_kbps, int preamble_bits,
                                    size_t payload_len);

#endif
