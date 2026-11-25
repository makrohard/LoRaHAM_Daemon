/*
 * radio_units.h: the daemon's MHz / kHz / kbps floats to RadioLib 8's integer
 * Hz / bps, and the TCXO voltage to RadioLib's enum.
 *
 * Every value the daemon's validation accepts from a list (LoRa BW, the
 * SX126x RXBW raster) must arrive at RadioLib as the exact Hz figure RadioLib
 * looks up; the band defaults and the FSK boot values are pinned as they
 * convert (the nearest Hz to the float the daemon holds); every hardware
 * profile's TCXO voltage must map; and anything else is refused, not guessed.
 */

#include "../radio_units.h"
#include "../config_policy.h"
#include "../hardware_profile.h"

#include <stdio.h>
#include <string.h>

#include <string>

static int g_ok = 0;
static int g_fail = 0;

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want) {
        g_ok++;
        printf("[ OK ] %s = %u\n", name, (unsigned)got);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %u, got %u\n", name, (unsigned)want, (unsigned)got);
    }
}

static void expect_true(const char *name, bool cond)
{
    if (cond) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
}

/* LoRa bandwidths: config_policy's list, and the Hz RadioLib 8 takes. */
static void test_lora_bandwidths(void)
{
    static const struct { float khz; uint32_t hz; } bw[] = {
        { 7.8f, 7800 }, { 10.4f, 10400 }, { 15.6f, 15600 }, { 20.8f, 20800 },
        { 31.25f, 31250 }, { 41.7f, 41700 }, { 62.5f, 62500 }, { 125.0f, 125000 },
        { 250.0f, 250000 }, { 500.0f, 500000 },
    };
    for (const auto &b : bw) {
        char name[64];
        snprintf(name, sizeof(name), "LoRa BW %.2f kHz", b.khz);
        expect_true((std::string(name) + " is a policy value").c_str(),
                    config_policy_lora_bandwidth_valid(b.khz));
        expect_u32(name, radio_hz_from_khz(b.khz), b.hz);
    }
}

/* The SX126x GFSK RX bandwidth raster: the policy's list, exact Hz. */
static void test_sx126x_rx_bandwidths(void)
{
    static const struct { float khz; uint32_t hz; } bw[] = {
        { 4.8f, 4800 }, { 5.8f, 5800 }, { 7.3f, 7300 }, { 9.7f, 9700 }, { 11.7f, 11700 },
        { 14.6f, 14600 }, { 19.5f, 19500 }, { 23.4f, 23400 }, { 29.3f, 29300 },
        { 39.0f, 39000 }, { 46.9f, 46900 }, { 58.6f, 58600 }, { 78.2f, 78200 },
        { 93.8f, 93800 }, { 117.3f, 117300 }, { 156.2f, 156200 }, { 187.2f, 187200 },
        { 234.3f, 234300 }, { 312.0f, 312000 }, { 373.6f, 373600 }, { 467.0f, 467000 },
    };
    for (const auto &b : bw) {
        char name[64];
        snprintf(name, sizeof(name), "SX126x RXBW %.1f kHz", b.khz);
        expect_true((std::string(name) + " is a policy value").c_str(),
                    config_policy_fsk_rxbw_valid_sx126x(b.khz));
        expect_u32(name, radio_hz_from_khz(b.khz), b.hz);
    }
}

/* Frequencies: the nearest Hz to the float the daemon holds (433.9f is
 * 433.899993896 MHz, so 433899994 Hz); the register effect is pinned by
 * test_radio_register_image. */
static void test_frequencies(void)
{
    expect_u32("433 band default 433.9", radio_hz_from_mhz(433.9f), 433899994u);
    expect_u32("868 band default 869.525", radio_hz_from_mhz(869.525f), 869525024u);
    expect_u32("915 band default 906.875", radio_hz_from_mhz(906.875f), 906875000u);
    expect_u32("434.0", radio_hz_from_mhz(434.0f), 434000000u);
    expect_u32("FREQ 0 -> 0 (RadioLib refuses it)", radio_hz_from_mhz(0.0f), 0u);
    expect_u32("FREQ -1 -> 0", radio_hz_from_mhz(-1.0f), 0u);
    expect_u32("BW 0 -> 0", radio_hz_from_khz(0.0f), 0u);
}

/* FSK: the boot values and the bit rate / deviation range ends. */
static void test_fsk_values(void)
{
    expect_u32("BR 4.8 kbps", radio_bps_from_kbps(4.8f), 4800u);
    expect_u32("BR 0.6 kbps", radio_bps_from_kbps(0.6f), 600u);
    expect_u32("BR 1.2 kbps", radio_bps_from_kbps(1.2f), 1200u);
    expect_u32("BR 300 kbps", radio_bps_from_kbps(300.0f), 300000u);
    expect_u32("FREQDEV 5 kHz", radio_hz_from_khz(5.0f), 5000u);
    expect_u32("FREQDEV 0.6 kHz", radio_hz_from_khz(0.6f), 600u);
    expect_u32("FREQDEV 200 kHz", radio_hz_from_khz(200.0f), 200000u);
}

static void expect_tcxo(const char *name, float volts, bool ok, RadioLibTCXOVoltage_t want)
{
    RadioLibTCXOVoltage_t got = Voltage3V3;
    bool r = radio_tcxo_voltage(volts, &got);
    if (r == ok && (!ok || got == want)) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: ok=%d code=%d (want ok=%d code=%d)\n", name, (int)r, (int)got,
               (int)ok, (int)want);
    }
}

static void test_tcxo_map(void)
{
    expect_tcxo("TCXO 0 V = crystal (VoltageNone)", 0.0f, true, VoltageNone);
    expect_tcxo("TCXO -1 V = crystal (VoltageNone)", -1.0f, true, VoltageNone);
    expect_tcxo("TCXO 1.6 V", 1.6f, true, Voltage1V6);
    expect_tcxo("TCXO 1.7 V", 1.7f, true, Voltage1V7);
    expect_tcxo("TCXO 1.8 V", 1.8f, true, Voltage1V8);
    expect_tcxo("TCXO 2.2 V", 2.2f, true, Voltage2V2);
    expect_tcxo("TCXO 2.4 V", 2.4f, true, Voltage2V4);
    expect_tcxo("TCXO 2.7 V", 2.7f, true, Voltage2V7);
    expect_tcxo("TCXO 3.0 V", 3.0f, true, Voltage3V0);
    expect_tcxo("TCXO 3.3 V", 3.3f, true, Voltage3V3);
    expect_tcxo("TCXO 1.8005 V (within 1 mV)", 1.8005f, true, Voltage1V8);
    expect_tcxo("TCXO 1.802 V refused", 1.802f, false, VoltageNone);
    expect_tcxo("TCXO 1.75 V refused", 1.75f, false, VoltageNone);
    expect_tcxo("TCXO 3.4 V refused", 3.4f, false, VoltageNone);
}

/* Every SX1262 hardware profile, every band it serves: its voltage maps. */
static void test_every_profile_voltage_maps(void)
{
    std::string known = daemon_hardware_profile_known();
    size_t pos = 0;
    int checked = 0;
    while (pos < known.size()) {
        size_t end = known.find(',', pos);
        std::string name = known.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        while (!name.empty() && name[0] == ' ')
            name.erase(0, 1);
        for (int band : {433, 868, 915}) {
            daemon_set_hardware_preset(name.c_str());
            if (!daemon_hardware_profile_resolve(band) ||
                daemon_hw_profile.family != DAEMON_CHIP_FAMILY_SX1262)
                continue;
            RadioLibTCXOVoltage_t v;
            char label[96];
            snprintf(label, sizeof(label), "profile %s/%d TCXO %.2f V maps", name.c_str(), band,
                     daemon_hw_profile.tcxo_voltage);
            expect_true(label, radio_tcxo_voltage(daemon_hw_profile.tcxo_voltage, &v));
            checked++;
        }
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    expect_true("at least one SX1262 profile checked", checked > 0);
}

int main(void)
{
    test_lora_bandwidths();
    test_sx126x_rx_bandwidths();
    test_frequencies();
    test_fsk_values();
    test_tcxo_map();
    test_every_profile_voltage_maps();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail == 0 ? 0 : 1;
}
