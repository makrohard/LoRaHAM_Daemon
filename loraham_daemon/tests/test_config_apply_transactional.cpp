#include "../config_apply.h"

#include <stdio.h>
#include <atomic>
#include <string.h>

/* --- Transactional CONFIG apply integration tests ------------------------ */

static int g_ok = 0;
static int g_fail = 0;

// Fake-Treiber: MODE-Wechsel und Parameter-Apply laufen über die virtuellen
// RadioDriver-Methoden; die Zähler-Semantik entspricht dem alten
// Template-Fake (begin()/beginFSK()-Zähler + Apply-Zähler).
struct FakeRadio : public RadioDriver {
    int begin_count;
    int begin_fsk_count;
    int lora_apply_count;
    int fsk_apply_count;
    int begin_result;
    int begin_fsk_result;

    FakeRadio() :
        RadioDriver(NULL),
        begin_count(0),
        begin_fsk_count(0),
        lora_apply_count(0),
        fsk_apply_count(0),
        begin_result(RADIOLIB_ERR_NONE),
        begin_fsk_result(RADIOLIB_ERR_NONE)
    {
    }

    int16_t switchMode(RadioMode_t mode) override
    {
        if (mode == RADIO_MODE_FSK) {
            begin_fsk_count++;
            return begin_fsk_result;
        }

        begin_count++;
        return begin_result;
    }

    void applyLoraParam(const char *, const std::string &,
                        const std::string &) override
    {
        lora_apply_count++;
    }

    void applyFskParam(const char *, const std::string &,
                       const std::string &) override
    {
        fsk_apply_count++;
    }

    int16_t begin(const RadioRfDefaults *) override { return begin_result; }
    int16_t clearIrq(uint32_t) override { return 0; }
    float readLiveRssi(RadioMode_t, bool) override { return -200.0f; }
    float rssiProbe() override { return -200.0f; }
    const char *chipName() const override { return "FAKE"; }
    DaemonChipFamily chipFamily() const override
    {
        return DAEMON_CHIP_FAMILY_SX127X;
    }
};

/* --- Test helpers --- */

static void expect_int(const char *name, int actual, int expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %d, got %d\n",
               name, expected, actual);
    }
}

static void apply_cmd(FakeRadio &radio,
                      const char *cmd,
                      RadioMode_t &mode,
                      std::atomic<bool> &getrssi)
{
    parse_and_apply_config_generic(radio, "TEST", cmd, mode, getrssi);
}

/* --- Tests --------------------------------------------------------------- */

static void test_invalid_getrssi_has_no_side_effects(void)
{
    FakeRadio radio;
    RadioMode_t mode = RADIO_MODE_LORA;
    std::atomic<bool> getrssi(false);

    apply_cmd(radio, "SET GETRSSI=2 MODE=FSK", mode, getrssi);

    expect_int("invalid getrssi mode unchanged", mode, RADIO_MODE_LORA);
    expect_int("invalid getrssi flag unchanged", getrssi.load(), false);
    expect_int("invalid getrssi beginFSK not called", radio.begin_fsk_count, 0);
    expect_int("invalid getrssi lora apply not called", radio.lora_apply_count, 0);
    expect_int("invalid getrssi fsk apply not called", radio.fsk_apply_count, 0);
}

static void test_invalid_fsk_value_has_no_mode_or_getrssi_side_effects(void)
{
    FakeRadio radio;
    RadioMode_t mode = RADIO_MODE_LORA;
    std::atomic<bool> getrssi(false);

    apply_cmd(radio, "SET MODE=FSK GETRSSI=1 BR=bad", mode, getrssi);

    expect_int("invalid fsk mode unchanged", mode, RADIO_MODE_LORA);
    expect_int("invalid fsk getrssi unchanged", getrssi.load(), false);
    expect_int("invalid fsk beginFSK not called", radio.begin_fsk_count, 0);
    expect_int("invalid fsk fsk apply not called", radio.fsk_apply_count, 0);
}

static void test_malformed_token_has_no_side_effects(void)
{
    FakeRadio radio;
    RadioMode_t mode = RADIO_MODE_LORA;
    std::atomic<bool> getrssi(false);

    apply_cmd(radio, "SET MODE=FSK GETRSSI=1 BROKEN", mode, getrssi);

    expect_int("malformed mode unchanged", mode, RADIO_MODE_LORA);
    expect_int("malformed getrssi unchanged", getrssi.load(), false);
    expect_int("malformed beginFSK not called", radio.begin_fsk_count, 0);
    expect_int("malformed fsk apply not called", radio.fsk_apply_count, 0);
}


static void test_failed_mode_switch_aborts_remaining_apply(void)
{
    FakeRadio radio;
    RadioMode_t mode = RADIO_MODE_LORA;
    std::atomic<bool> getrssi(false);

    radio.begin_fsk_result = -123;

    apply_cmd(radio, "SET MODE=FSK GETRSSI=1 BR=4.8 RXBW=12.5",
              mode, getrssi);

    expect_int("failed mode switch beginFSK called", radio.begin_fsk_count, 1);
    expect_int("failed mode switch keeps old mode", mode, RADIO_MODE_LORA);
    expect_int("failed mode switch leaves getrssi unchanged", getrssi.load(), false);
    expect_int("failed mode switch skips lora apply", radio.lora_apply_count, 0);
    expect_int("failed mode switch skips fsk apply", radio.fsk_apply_count, 0);
}

static void test_valid_mode_and_getrssi_still_apply(void)
{
    FakeRadio radio;
    RadioMode_t mode = RADIO_MODE_LORA;
    std::atomic<bool> getrssi(false);

    apply_cmd(radio, "SET MODE=FSK GETRSSI=1", mode, getrssi);

    expect_int("valid mode switched", mode, RADIO_MODE_FSK);
    expect_int("valid getrssi set", getrssi.load(), true);
    expect_int("valid beginFSK called", radio.begin_fsk_count, 1);
}

static void test_valid_lora_parameter_still_applies(void)
{
    FakeRadio radio;
    RadioMode_t mode = RADIO_MODE_LORA;
    std::atomic<bool> getrssi(false);

    apply_cmd(radio, "SET FREQ=433.900", mode, getrssi);

    expect_int("valid lora param apply count", radio.lora_apply_count, 1);
    expect_int("valid lora mode unchanged", mode, RADIO_MODE_LORA);
    expect_int("valid lora getrssi unchanged", getrssi.load(), false);
}

// SX1262-Variante: das Familien-Raster der Validierung folgt dem Treiber.
struct FakeSx1262Radio : public FakeRadio {
    DaemonChipFamily chipFamily() const override
    {
        return DAEMON_CHIP_FAMILY_SX1262;
    }
};

static void test_fsk_rxbw_follows_driver_family(void)
{
    // SX126x raster value: rejected on the (default) SX127x family fake...
    {
        FakeRadio radio;
        RadioMode_t mode = RADIO_MODE_LORA;
        std::atomic<bool> getrssi(false);

        apply_cmd(radio, "SET MODE=FSK BR=9.6 RXBW=23.4", mode, getrssi);
        expect_int("sx127x family rejects 23.4 (no mode switch)",
                   radio.begin_fsk_count, 0);
        expect_int("sx127x family rejects 23.4 (no apply)",
                   radio.fsk_apply_count, 0);
    }

    // ...accepted transactionally on the SX1262 family fake...
    {
        FakeSx1262Radio radio;
        RadioMode_t mode = RADIO_MODE_LORA;
        std::atomic<bool> getrssi(false);

        apply_cmd(radio, "SET MODE=FSK BR=9.6 RXBW=23.4", mode, getrssi);
        expect_int("sx1262 family accepts 23.4 (mode switched)",
                   radio.begin_fsk_count, 1);
        expect_int("sx1262 family accepts 23.4 (params applied)",
                   radio.fsk_apply_count, 2);
    }

    // ...and an SX127x raster value rejects the whole command on SX1262.
    {
        FakeSx1262Radio radio;
        RadioMode_t mode = RADIO_MODE_LORA;
        std::atomic<bool> getrssi(false);

        apply_cmd(radio, "SET MODE=FSK BR=9.6 RXBW=25.0", mode, getrssi);
        expect_int("sx1262 family rejects 25.0 (no mode switch)",
                   radio.begin_fsk_count, 0);
        expect_int("sx1262 family rejects 25.0 (no apply)",
                   radio.fsk_apply_count, 0);
        expect_int("sx1262 family rejects 25.0 (mode flag unchanged)",
                   mode, RADIO_MODE_LORA);
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bin") == 0) {
            if (i + 1 >= argc) {
                printf("Usage: %s [--bin ignored]\n", argv[0]);
                return 2;
            }
            i++;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--bin ignored]\n", argv[0]);
            return 0;
        } else {
            printf("Usage: %s [--bin ignored]\n", argv[0]);
            return 2;
        }
    }

    test_invalid_getrssi_has_no_side_effects();
    test_invalid_fsk_value_has_no_mode_or_getrssi_side_effects();
    test_malformed_token_has_no_side_effects();
    test_failed_mode_switch_aborts_remaining_apply();
    test_valid_mode_and_getrssi_still_apply();
    test_valid_lora_parameter_still_applies();
    test_fsk_rxbw_follows_driver_family();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
