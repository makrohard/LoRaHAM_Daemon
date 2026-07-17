#include "config_policy.h"

#include <stddef.h>

#include "config_value.h"

/* --- LoRa CONFIG value policy ------------------------------------------- */

bool config_policy_lora_sf_valid(int sf)
{
    return sf >= 7 && sf <= 12;
}

bool config_policy_lora_bandwidth_valid(float bw)
{
    const float allowed[] = {
        7.8f, 10.4f, 15.6f, 20.8f, 31.25f,
        41.7f, 62.5f, 125.0f, 250.0f, 500.0f
    };

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (config_value_float_equal(bw, allowed[i]))
            return true;
    }

    return false;
}

bool config_policy_lora_cr_valid(int cr)
{
    return cr >= 5 && cr <= 8;
}

/* Cap at 512 symbols (audit P1-2): the old 65535 ceiling allowed a single
 * valid preamble of ~36 min at SF12/BW125 — unstoppable past the systemd
 * stop timeout. 512 covers every real profile (boot uses 8/16) while
 * bounding the worst case; a full airtime-based policy stays deferred. */
bool config_policy_lora_preamble_valid(int preamble)
{
    return preamble >= 6 && preamble <= 512;
}

bool config_policy_lora_sync_valid(uint32_t sync)
{
    return sync <= 0xFF;
}

bool config_policy_power_valid(int power)
{
    return power >= 0 && power <= 20;
}

/* --- FSK CONFIG value policy -------------------------------------------- */

bool config_policy_fsk_bitrate_valid(float br)
{
    return br >= 0.5f && br <= 300.0f;
}

bool config_policy_fsk_freqdev_valid(float freqdev)
{
    return freqdev > 0.0f && freqdev <= 200.0f;
}

bool config_policy_fsk_rxbw_valid(float bw)
{
    const float allowed[] = {
        2.6f, 3.1f, 3.9f, 5.2f, 6.3f, 7.8f,
        10.4f, 12.5f, 15.6f, 20.8f, 25.0f,
        31.25f, 31.3f, 41.7f, 50.0f, 62.5f,
        83.3f, 100.0f, 125.0f, 166.7f,
        200.0f, 250.0f
    };

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (config_value_float_equal(bw, allowed[i]))
            return true;
    }

    return false;
}

/* SX126x GFSK RX bandwidth raster (RadioLib SX126x::setRxBandwidth). The two
 * chip rasters barely intersect, so the validator must select per family. */
bool config_policy_fsk_rxbw_valid_sx126x(float bw)
{
    const float allowed[] = {
        4.8f, 5.8f, 7.3f, 9.7f, 11.7f, 14.6f,
        19.5f, 23.4f, 29.3f, 39.0f, 46.9f,
        58.6f, 78.2f, 93.8f, 117.3f, 156.2f,
        187.2f, 234.3f, 312.0f, 373.6f, 467.0f
    };

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (config_value_float_equal(bw, allowed[i]))
            return true;
    }

    return false;
}

bool config_policy_fsk_rxbw_valid_family(float bw, DaemonChipFamily family)
{
    if (family == DAEMON_CHIP_FAMILY_SX1262)
        return config_policy_fsk_rxbw_valid_sx126x(bw);

    return config_policy_fsk_rxbw_valid(bw);
}

bool config_policy_fsk_preamble_valid(int preamble)
{
    return preamble >= 0 && preamble <= 2048;
}

bool config_policy_fsk_sync_valid(uint32_t sync)
{
    if (sync == 0 || sync > 0xFFFF)
        return false;

    if (sync <= 0xFF)
        return sync != 0x00;

    return ((sync >> 8) & 0xFF) != 0x00 &&
           (sync & 0xFF) != 0x00;
}

/* Band frequency policy (audit P1-4): limits come from the immutable band
 * descriptor; validation receives them explicitly so this layer stays free
 * of daemon globals. */
bool config_policy_freq_valid_band(float freq_mhz,
                                   float min_mhz,
                                   float max_mhz)
{
    if (!(min_mhz > 0.0f) || !(max_mhz > min_mhz))
        return false;

    return freq_mhz >= min_mhz && freq_mhz <= max_mhz;
}

/* Family capability policy (audit P1-3): prevalidation must reject what the
 * concrete driver will reject, or "whole-command prevalidation" is a lie
 * (SET MODE=FSK OOK=1 would switch mode and only then fail the key). */

bool config_policy_fsk_freqdev_valid_family(float freqdev,
                                            DaemonChipFamily family)
{
    if (!config_policy_fsk_freqdev_valid(freqdev))
        return false;

    /* SX126x rejects positive deviations below 0.6 kHz. */
    if (family == DAEMON_CHIP_FAMILY_SX1262)
        return freqdev >= 0.6f;

    return true;
}

bool config_policy_fsk_ook_valid_family(int ook, DaemonChipFamily family)
{
    /* SX126x has no OOK modulator; only OOK=0 (off) is acceptable. */
    if (family == DAEMON_CHIP_FAMILY_SX1262)
        return ook == 0;

    return ook == 0 || ook == 1;
}

bool config_policy_fsk_encoding_valid_family(int encoding,
                                             DaemonChipFamily family)
{
    if (encoding < 0 || encoding > 2)
        return false;

    /* SX126x maps setEncoding to whitening only: 1 (Manchester) would
     * silently enable whitening instead — accept only 0 and 2. */
    if (family == DAEMON_CHIP_FAMILY_SX1262)
        return encoding != 1;

    return true;
}
