#include "../hardware_profile.h"

#include <stdio.h>
#include <string.h>

/* --- Hardware profile preset tests --------------------------------------- */

static int g_ok = 0;
static int g_fail = 0;

static void expect_int(const char *name, long actual, long expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %ld, got %ld\n", name, expected, actual);
    }
}

static void expect_str(const char *name, const char *actual, const char *expected)
{
    if (actual && strcmp(actual, expected) == 0) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %s, got %s\n", name, expected,
               actual ? actual : "(null)");
    }
}

static int claimed_contains(const DaemonHardwareProfile *p, int pin)
{
    for (int i = 0; i < p->claimed_count; i++) {
        if (p->claimed[i] == pin)
            return 1;
    }
    return 0;
}

/* loraham (canonical) must resolve per band exactly to the LoRaHAM_Pi wiring. */
static void test_loraham_433(void)
{
    expect_int("set loraham", daemon_set_hardware_preset("loraham"), 1);
    expect_int("resolve loraham 433", daemon_hardware_profile_resolve(433), 1);
    expect_str("loraham name", daemon_hw_profile.name, "loraham");
    expect_int("legacy family sx127x", daemon_hw_profile.family,
               DAEMON_CHIP_FAMILY_SX127X);
    expect_int("legacy 433 cs", daemon_hw_profile.cs, 8);
    expect_int("legacy 433 irq/dio0", daemon_hw_profile.irq, 25);
    expect_int("legacy 433 rst", daemon_hw_profile.rst, 5);
    expect_int("legacy 433 gpio/dio1", daemon_hw_profile.gpio, 24);
    expect_int("legacy 433 led", daemon_hw_profile.led_pin, 13);
    expect_int("legacy cad scan", daemon_hw_profile.cad_scan_available, 1);
    expect_int("legacy fsk stream", daemon_hw_profile.fsk_stream_available, 1);
    expect_int("legacy reset wired", daemon_hw_profile.reset_wired, 1);
}

static void test_loraham_868(void)
{
    expect_int("resolve legacy 868", daemon_hardware_profile_resolve(868), 1);
    expect_int("legacy 868 cs", daemon_hw_profile.cs, 7);
    expect_int("legacy 868 irq/dio0", daemon_hw_profile.irq, 16);
    expect_int("legacy 868 rst", daemon_hw_profile.rst, 6);
    expect_int("legacy 868 gpio/dio1", daemon_hw_profile.gpio, 12);
    expect_int("legacy 868 led", daemon_hw_profile.led_pin, 19);
}


/* Uputronics: DIO1 and RESET not routed; LED slot-based; band-agnostic. */
static void test_uputronics_ce0(void)
{
    expect_int("set ce0", daemon_set_hardware_preset("uputronics-ce0"), 1);
    expect_int("resolve ce0 433", daemon_hardware_profile_resolve(433), 1);
    expect_str("ce0 name", daemon_hw_profile.name, "uputronics-ce0");
    expect_int("ce0 cs", daemon_hw_profile.cs, 8);
    expect_int("ce0 irq/dio0", daemon_hw_profile.irq, 25);
    expect_int("ce0 rst NC", daemon_hw_profile.rst < 0, 1);
    expect_int("ce0 dio1 NC", daemon_hw_profile.gpio < 0, 1);
    expect_int("ce0 aux dio5 doc", daemon_hw_profile.aux, 24);
    expect_int("ce0 led", daemon_hw_profile.led_pin, 6);
    /* Register-polled CAD needs no DIO1, so this board has trustworthy
     * active CAD despite gpio=NC asserted just above. */
    expect_int("ce0 cad scan despite no DIO1",
               daemon_hw_profile.cad_scan_available, 1);
    expect_int("ce0 no fsk stream", daemon_hw_profile.fsk_stream_available, 0);
    expect_int("ce0 reset not wired", daemon_hw_profile.reset_wired, 0);
    expect_int("ce0 claims cs", claimed_contains(&daemon_hw_profile, 8), 1);
    expect_int("ce0 claims dio0", claimed_contains(&daemon_hw_profile, 25), 1);
    expect_int("ce0 claims led", claimed_contains(&daemon_hw_profile, 6), 1);
    expect_int("ce0 claims no rst", claimed_contains(&daemon_hw_profile, 5), 0);

    /* Same wiring for the 868 binding (band comes from --radio). */
    expect_int("resolve ce0 868", daemon_hardware_profile_resolve(868), 1);
    expect_int("ce0 868 cs unchanged", daemon_hw_profile.cs, 8);
}

static void test_uputronics_ce1(void)
{
    expect_int("set ce1", daemon_set_hardware_preset("uputronics-ce1"), 1);
    expect_int("resolve ce1 433", daemon_hardware_profile_resolve(433), 1);
    expect_int("ce1 cs", daemon_hw_profile.cs, 7);
    expect_int("ce1 irq/dio0", daemon_hw_profile.irq, 16);
    expect_int("ce1 rst NC", daemon_hw_profile.rst < 0, 1);
    expect_int("ce1 dio1 NC", daemon_hw_profile.gpio < 0, 1);
    expect_int("ce1 aux dio5 doc", daemon_hw_profile.aux, 12);
    expect_int("ce1 led", daemon_hw_profile.led_pin, 13);
    /* Register-polled CAD needs no DIO1, so this board has trustworthy
     * active CAD despite gpio=NC asserted just above. */
    expect_int("ce1 cad scan despite no DIO1",
               daemon_hw_profile.cad_scan_available, 1);
}

static void test_waveshare(void)
{
    expect_int("set waveshare", daemon_set_hardware_preset("waveshare-sx1262"), 1);
    expect_int("resolve waveshare 868", daemon_hardware_profile_resolve(868), 1);
    expect_int("ws family sx1262", daemon_hw_profile.family,
               DAEMON_CHIP_FAMILY_SX1262);
    expect_int("ws cs", daemon_hw_profile.cs, 21);
    expect_int("ws irq/dio1", daemon_hw_profile.irq, 16);
    expect_int("ws rst", daemon_hw_profile.rst, 18);
    expect_int("ws gpio/busy", daemon_hw_profile.gpio, 20);
    expect_int("ws txen", daemon_hw_profile.txen, 6);
    expect_int("ws rxen NC (DIO2 internal)", daemon_hw_profile.rxen < 0, 1);
    expect_int("ws tcxo set", daemon_hw_profile.tcxo_voltage > 0.0f, 1);
    expect_int("ws led NC", daemon_hw_profile.led_pin < 0, 1);
    expect_int("ws cad scan", daemon_hw_profile.cad_scan_available, 1);
    expect_int("ws led not claimed", claimed_contains(&daemon_hw_profile, 13), 0);
    expect_int("ws claims txen 6", claimed_contains(&daemon_hw_profile, 6), 1);

    /* LF and HF variants are pin-identical: 433 binding resolves the same. */
    expect_int("resolve waveshare 433", daemon_hardware_profile_resolve(433), 1);
    expect_int("ws 433 cs unchanged", daemon_hw_profile.cs, 21);
}

/* The ELECROW Meshstick: CH341 D-lines, not BCM; nothing claimed on the Pi;
 * 868 only. Every Pi preset stays a non-USB profile. */
static void test_meshstick(void)
{
    static const char *pi_presets[] = { "loraham", "uputronics-ce0", "uputronics-ce1",
                                        "waveshare-sx1262" };

    expect_int("set meshstick", daemon_set_hardware_preset("meshstick-1262"), 1);
    expect_int("meshstick 433 refused (868/915 hardware)", daemon_hardware_profile_resolve(433), 0);
    expect_int("resolve meshstick 868", daemon_hardware_profile_resolve(868), 1);
    expect_int("ms family sx1262", daemon_hw_profile.family, DAEMON_CHIP_FAMILY_SX1262);
    expect_int("ms usb", daemon_hw_profile.usb_ch341, 1);
    expect_int("ms vid 1A86", daemon_hw_profile.usb_vid, 0x1A86);
    expect_int("ms pid 5512", daemon_hw_profile.usb_pid, 0x5512);
    expect_int("ms cs D0", daemon_hw_profile.cs, 0);
    expect_int("ms irq/dio1 D6", daemon_hw_profile.irq, 6);
    expect_int("ms rst D2", daemon_hw_profile.rst, 2);
    expect_int("ms gpio/busy D4", daemon_hw_profile.gpio, 4);
    expect_int("ms rxen D1", daemon_hw_profile.rxen, 1);
    expect_int("ms txen NC", daemon_hw_profile.txen < 0, 1);
    expect_int("ms tcxo 1.8 V", daemon_hw_profile.tcxo_voltage == 1.8f, 1);
    expect_int("ms led NC", daemon_hw_profile.led_pin < 0, 1);
    expect_int("ms claims no Pi line", daemon_hw_profile.claimed_count, 0);
    expect_int("ms power ceiling 22 dBm", daemon_hw_profile.power_max_dbm, 22);
    expect_int("ms cad scan", daemon_hw_profile.cad_scan_available, 1);

    for (const char *name : pi_presets) {
        daemon_set_hardware_preset(name);
        static const int bands[] = { 433, 868 };
        for (int band : bands) {
            char label[96];
            snprintf(label, sizeof(label), "%s %d: not a USB profile", name, band);
            expect_int(label, daemon_hardware_profile_resolve(band) && !daemon_hw_profile.usb_ch341, 1);
            snprintf(label, sizeof(label), "%s %d: default power ceiling", name, band);
            expect_int(label, daemon_hw_profile.power_max_dbm, 0);
        }
    }
}

/* The PINE64 USB LoRa adapter (Pinedio): SX1262 behind the same CH341 as the
 * Meshstick; INT# is its DIO1, SLCT its BUSY (CH341 status bits 10 and 11, read
 * through the input word), no reset line, no TCXO; 868 only until the marking. */
static void test_pinedio_usb(void)
{
    expect_int("set pinedio", daemon_set_hardware_preset("pinedio-usb-1262"), 1);
    expect_int("pinedio 433 refused (868/915 hardware)", daemon_hardware_profile_resolve(433), 0);
    expect_int("resolve pinedio 868", daemon_hardware_profile_resolve(868), 1);
    expect_str("pd name", daemon_hw_profile.name, "pinedio-usb-1262");
    expect_int("pd family sx1262", daemon_hw_profile.family, DAEMON_CHIP_FAMILY_SX1262);
    expect_int("pd usb", daemon_hw_profile.usb_ch341, 1);
    expect_int("pd vid 1A86", daemon_hw_profile.usb_vid, 0x1A86);
    expect_int("pd pid 5512", daemon_hw_profile.usb_pid, 0x5512);
    expect_int("pd cs D0", daemon_hw_profile.cs, 0);
    expect_int("pd irq/dio1 INT# (10)", daemon_hw_profile.irq, 10);
    expect_int("pd rst none", daemon_hw_profile.rst < 0, 1);
    expect_int("pd reset not wired (warm start)", daemon_hw_profile.reset_wired, 0);
    expect_int("pd gpio/busy SLCT (11)", daemon_hw_profile.gpio, 11);
    expect_int("pd rxen NC", daemon_hw_profile.rxen < 0, 1);
    expect_int("pd txen NC", daemon_hw_profile.txen < 0, 1);
    expect_int("pd no tcxo", daemon_hw_profile.tcxo_voltage == 0.0f, 1);
    expect_int("pd led NC", daemon_hw_profile.led_pin < 0, 1);
    expect_int("pd claims no Pi line", daemon_hw_profile.claimed_count, 0);
    expect_int("pd power ceiling 14 dBm", daemon_hw_profile.power_max_dbm, 14);
    expect_int("pd cad scan", daemon_hw_profile.cad_scan_available, 1);
}

/* The CH341 + HopeRF RFM98PW 433 build: SX127x with PA_BOOST behind the CH341
 * (D0 NSS, D2 RESET, D6 DIO0, D4 DIO1); 433 only. High power is the
 * --high-power switch, as on every SX127x board; no FSK stream over USB. */
static void test_rfm98pw_433(void)
{
    expect_int("set rfm98pw", daemon_set_hardware_preset("ch341-rfm98pw-433"), 1);
    expect_int("rfm98pw 868 refused (433 hardware)", daemon_hardware_profile_resolve(868), 0);
    expect_int("resolve rfm98pw 433", daemon_hardware_profile_resolve(433), 1);
    expect_str("rf name", daemon_hw_profile.name, "ch341-rfm98pw-433");
    expect_int("rf family sx127x", daemon_hw_profile.family, DAEMON_CHIP_FAMILY_SX127X);
    expect_int("rf usb", daemon_hw_profile.usb_ch341, 1);
    expect_int("rf vid 1A86", daemon_hw_profile.usb_vid, 0x1A86);
    expect_int("rf pid 5512", daemon_hw_profile.usb_pid, 0x5512);
    expect_int("rf cs D0", daemon_hw_profile.cs, 0);
    expect_int("rf irq/dio0 D6", daemon_hw_profile.irq, 6);
    expect_int("rf rst D2", daemon_hw_profile.rst, 2);
    expect_int("rf reset wired", daemon_hw_profile.reset_wired, 1);
    expect_int("rf gpio/dio1 D4", daemon_hw_profile.gpio, 4);
    expect_int("rf rxen NC", daemon_hw_profile.rxen < 0, 1);
    expect_int("rf txen NC", daemon_hw_profile.txen < 0, 1);
    expect_int("rf no tcxo", daemon_hw_profile.tcxo_voltage == 0.0f, 1);
    expect_int("rf claims no Pi line", daemon_hw_profile.claimed_count, 0);
    expect_int("rf default power ceiling", daemon_hw_profile.power_max_dbm, 0);
    expect_int("rf no fsk stream over usb", daemon_hw_profile.fsk_stream_available, 0);
    expect_int("rf cad scan", daemon_hw_profile.cad_scan_available, 1);
}

/* Which band each preset serves (W237: the two USB SX1262 sticks also serve 915,
 * every other preset refuses it), and a band that is none of 433/868/915 is
 * refused by every preset. Before, loraham took its 868 wiring for any band
 * other than 433, and the uputronics and waveshare presets took any band. */
static void test_band_matrix(void)
{
    static const struct { const char *preset; bool b433, b868, b915; } rows[] = {
        { "loraham",           true,  true,  false },
        { "uputronics-ce0",    true,  true,  false },
        { "uputronics-ce1",    true,  true,  false },
        { "waveshare-sx1262",  true,  true,  false },
        { "meshstick-1262",    false, true,  true  },
        { "pinedio-usb-1262",  false, true,  true  },
        { "ch341-rfm98pw-433", true,  false, false },
    };
    static const int never[] = { 0, -1, 434, 869, 900, 2400 };

    for (const auto &r : rows) {
        char label[96];
        daemon_set_hardware_preset(r.preset);
        snprintf(label, sizeof(label), "band matrix %s 433", r.preset);
        expect_int(label, daemon_hardware_profile_resolve(433), r.b433);
        snprintf(label, sizeof(label), "band matrix %s 868", r.preset);
        expect_int(label, daemon_hardware_profile_resolve(868), r.b868);
        snprintf(label, sizeof(label), "band matrix %s 915", r.preset);
        expect_int(label, daemon_hardware_profile_resolve(915), r.b915);
        for (int band : never) {
            snprintf(label, sizeof(label), "band matrix %s %d refused", r.preset, band);
            expect_int(label, daemon_hardware_profile_resolve(band), 0);
        }
    }
}

/* On 915 the two sticks keep their wiring: the band changes nothing else. */
static void test_usb_sx1262_915_same_profile(void)
{
    static const char *sticks[] = { "meshstick-1262", "pinedio-usb-1262" };

    for (const char *name : sticks) {
        char label[96];
        daemon_set_hardware_preset(name);
        daemon_hardware_profile_resolve(868);
        DaemonHardwareProfile p868 = daemon_hw_profile;
        snprintf(label, sizeof(label), "%s 915 resolves", name);
        expect_int(label, daemon_hardware_profile_resolve(915), 1);
        snprintf(label, sizeof(label), "%s 915 = its 868 profile", name);
        expect_int(label, memcmp(&p868, &daemon_hw_profile, sizeof(p868)) == 0, 1);
    }
}

static void test_known_lists_every_preset(void)
{
    expect_str("known presets", daemon_hardware_profile_known(),
               "loraham, uputronics-ce0, uputronics-ce1, waveshare-sx1262, meshstick-1262, "
               "pinedio-usb-1262, ch341-rfm98pw-433");
}

static void test_unknown_preset(void)
{
    expect_int("set unknown records name",
               daemon_set_hardware_preset("does-not-exist"), 1);
    expect_int("resolve unknown fails", daemon_hardware_profile_resolve(433), 0);
    expect_int("set null fails", daemon_set_hardware_preset(NULL), 0);
    expect_int("set empty fails", daemon_set_hardware_preset(""), 0);
}

static void test_family_names(void)
{
    expect_str("family name sx127x",
               daemon_chip_family_name(DAEMON_CHIP_FAMILY_SX127X), "SX127x");
    expect_str("family name sx1262",
               daemon_chip_family_name(DAEMON_CHIP_FAMILY_SX1262), "SX1262");
}

/* The chip-select mode, the same four rows in every bus user: no kernel
 * chip selects / CS 8 / CS 7 / any other CS. */
static void test_spi_soft_cs_keeps_the_profile(void)
{
    daemon_set_hardware_preset("loraham");
    daemon_hardware_profile_resolve(868);
    expect_int("soft: applied", daemon_hardware_profile_apply_spi(&daemon_hw_profile, false), 1);
    expect_int("soft: cs stays GPIO 7", daemon_hw_profile.cs, 7);
    expect_int("soft: spidev0.0", daemon_hw_profile.spi_channel, 0);
    expect_int("soft: not kernel", daemon_hw_profile.kernel_cs, 0);
    expect_int("soft: GPIO 7 claimed", claimed_contains(&daemon_hw_profile, 7), 1);
}

static void test_spi_kernel_cs8_uses_spidev00_without_the_gpio(void)
{
    daemon_set_hardware_preset("uputronics-ce0");
    daemon_hardware_profile_resolve(433);
    expect_int("ce0: applied", daemon_hardware_profile_apply_spi(&daemon_hw_profile, true), 1);
    expect_int("ce0: cs still names BCM 8", daemon_hw_profile.cs, 8);
    expect_int("ce0: spidev0.0", daemon_hw_profile.spi_channel, 0);
    expect_int("ce0: kernel", daemon_hw_profile.kernel_cs, 1);
    expect_int("ce0: GPIO 8 not claimed", claimed_contains(&daemon_hw_profile, 8), 0);
    expect_int("ce0: IRQ 25 still claimed", claimed_contains(&daemon_hw_profile, 25), 1);
}

static void test_spi_kernel_cs7_uses_spidev01_without_the_gpio(void)
{
    daemon_set_hardware_preset("loraham");
    daemon_hardware_profile_resolve(868);
    expect_int("ce1: applied", daemon_hardware_profile_apply_spi(&daemon_hw_profile, true), 1);
    expect_int("ce1: cs still names BCM 7", daemon_hw_profile.cs, 7);
    expect_int("ce1: spidev0.1", daemon_hw_profile.spi_channel, 1);
    expect_int("ce1: GPIO 7 not claimed", claimed_contains(&daemon_hw_profile, 7), 0);
    expect_int("ce1: LED 19 still claimed", claimed_contains(&daemon_hw_profile, 19), 1);
}

static void test_spi_kernel_cs_refuses_a_cs_off_ce0_ce1(void)
{
    daemon_set_hardware_preset("waveshare-sx1262");
    daemon_hardware_profile_resolve(433);
    expect_int("cs21: refused", daemon_hardware_profile_apply_spi(&daemon_hw_profile, true), 0);
}

int main(void)
{
    test_band_matrix();
    test_usb_sx1262_915_same_profile();
    test_loraham_433();
    test_loraham_868();
    test_uputronics_ce0();
    test_uputronics_ce1();
    test_waveshare();
    test_meshstick();
    test_pinedio_usb();
    test_rfm98pw_433();
    test_known_lists_every_preset();
    test_unknown_preset();
    test_family_names();
    test_spi_soft_cs_keeps_the_profile();
    test_spi_kernel_cs8_uses_spidev00_without_the_gpio();
    test_spi_kernel_cs7_uses_spidev01_without_the_gpio();
    test_spi_kernel_cs_refuses_a_cs_off_ce0_ce1();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
