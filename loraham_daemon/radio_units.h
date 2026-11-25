#ifndef LORAHAM_RADIO_UNITS_H
#define LORAHAM_RADIO_UNITS_H

#include <math.h>
#include <stdint.h>

#include <RadioLib.h>

/* --- The daemon's units at the RadioLib boundary ---------------------------- */
/*
 * The daemon's protocol, policy, STATUS and replay store keep frequency in MHz,
 * bandwidths and FSK deviation in kHz and the FSK bit rate in kbps, as floats.
 * RadioLib 8 takes all of them as integers in Hz (bit rate in bps). These are
 * the only conversions, applied at each RadioLib call: the value rounded to the
 * nearest Hz from the float the daemon holds. A value that is not above zero
 * converts to 0, which RadioLib refuses as out of range (the daemon's own
 * validation never passes one).
 */
static inline uint32_t radio_hz_from_mhz(float mhz)
{
    return mhz > 0.0f ? (uint32_t)lround((double)mhz * 1e6) : 0u;
}

static inline uint32_t radio_hz_from_khz(float khz)
{
    return khz > 0.0f ? (uint32_t)lround((double)khz * 1e3) : 0u;
}

/* kbps -> bps: the same factor as kHz -> Hz. */
static inline uint32_t radio_bps_from_kbps(float kbps)
{
    return radio_hz_from_khz(kbps);
}

/*
 * The SX126x TCXO reference voltage on DIO3. The hardware profiles hold it as a
 * float: 0 (or less) = a crystal, no TCXO; otherwise one of the eight voltages
 * the chip offers, matched within 1 mV as RadioLib 7's setTCXO() did. Any other
 * value is refused (false), and begin() returns RADIOLIB_ERR_INVALID_TCXO_VOLTAGE
 * without touching the chip, which is what RadioLib 7 returned for it.
 */
static inline bool radio_tcxo_voltage(float volts, RadioLibTCXOVoltage_t *out)
{
    static const struct { float volts; RadioLibTCXOVoltage_t code; } table[] = {
        { 1.6f, Voltage1V6 }, { 1.7f, Voltage1V7 }, { 1.8f, Voltage1V8 },
        { 2.2f, Voltage2V2 }, { 2.4f, Voltage2V4 }, { 2.7f, Voltage2V7 },
        { 3.0f, Voltage3V0 }, { 3.3f, Voltage3V3 },
    };

    if (volts <= 0.0f) {
        *out = VoltageNone;
        return true;
    }
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (fabsf(volts - table[i].volts) <= 0.001f) {
            *out = table[i].code;
            return true;
        }
    }
    return false;
}

#endif
