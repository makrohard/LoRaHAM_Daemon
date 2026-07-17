#include "../config_apply.h"
#include "../daemon_band.h"

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
    int16_t lora_apply_result = 0;
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

    const RadioRfDefaults *last_switch_defaults = NULL;
    int call_seq = 0;
    int switch_seq = -1;
    int first_apply_seq = -1;

    int16_t switchMode(RadioMode_t mode,
                       const RadioRfDefaults *defaults) override
    {
        last_switch_defaults = defaults;
        switch_seq = call_seq++;

        if (mode == RADIO_MODE_FSK) {
            begin_fsk_count++;
            return begin_fsk_result;
        }

        begin_count++;
        return begin_result;
    }

    int16_t applyLoraParam(const char *, const std::string &,
                           const std::string &) override
    {
        if (first_apply_seq < 0)
            first_apply_seq = call_seq;
        call_seq++;
        lora_apply_count++;
        return lora_apply_result;
    }

    int16_t applyFskParam(const char *, const std::string &,
                          const std::string &) override
    {
        fsk_apply_count++;
        return 0;
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

/* --- SET MODE lands on the band boot defaults (audit M2) ------------------ */

static void test_mode_switch_passes_band_defaults(void)
{
    // 868 process: MODE switch must carry 869.525, never chip defaults.
    {
        FakeRadio radio;
        RadioMode_t mode = RADIO_MODE_FSK;
        std::atomic<bool> getrssi(false);

        daemon_band_resolve(RADIO_BAND_868);
        apply_cmd(radio, "SET MODE=LORA", mode, getrssi);

        expect_int("868 lora switch got defaults",
                   radio.last_switch_defaults != NULL, 1);
        expect_int("868 lora switch freq",
                   (int)(radio.last_switch_defaults->freq_mhz * 1000.0f + 0.5f),
                   869525);

        apply_cmd(radio, "SET MODE=FSK", mode, getrssi);
        expect_int("868 fsk switch freq",
                   (int)(radio.last_switch_defaults->freq_mhz * 1000.0f + 0.5f),
                   869525);
    }

    // 433 process: MODE switch must carry 433.900.
    {
        FakeRadio radio;
        RadioMode_t mode = RADIO_MODE_FSK;
        std::atomic<bool> getrssi(false);

        daemon_band_resolve(RADIO_BAND_433);
        apply_cmd(radio, "SET MODE=LORA", mode, getrssi);

        expect_int("433 lora switch got defaults",
                   radio.last_switch_defaults != NULL, 1);
        expect_int("433 lora switch freq",
                   (int)(radio.last_switch_defaults->freq_mhz * 1000.0f + 0.5f),
                   433900);
        expect_int("433 lora switch sf",
                   radio.last_switch_defaults->spreading_factor, 12);
    }
}

static void test_mode_switch_applies_before_explicit_params(void)
{
    // Combined command: MODE lands on band defaults FIRST, then the explicit
    // params override them — order is the guarantee that FREQ/SF win.
    FakeRadio radio;
    RadioMode_t mode = RADIO_MODE_FSK;
    std::atomic<bool> getrssi(false);

    daemon_band_resolve(RADIO_BAND_433);
    apply_cmd(radio, "SET MODE=LORA FREQ=433.175 SF=11", mode, getrssi);

    expect_int("combined switch happened", radio.begin_count, 1);
    expect_int("combined params applied", radio.lora_apply_count, 2);
    expect_int("combined switch before params",
               radio.switch_seq >= 0 &&
               radio.first_apply_seq > radio.switch_seq, 1);
}

/* Audit P1-7: merged current+command worst-case airtime gates the whole
 * command before any side effect; the shadow persists across commands. */
static void test_airtime_gate_merged_config(void)
{
    FakeRadio radio;
    RadioMode_t mode = RADIO_MODE_LORA;
    std::atomic<bool> getrssi(false);

    daemon_band_resolve(RADIO_BAND_433); /* shadow base: SF12/BW125/PRE8 */
    config_apply_effective_reset();

    /* BW=7.8 with the inherited SF12 shadow: ~145 s — rejected, no apply. */
    ConfigApplyStatus st =
        parse_and_apply_config_generic(radio, "TEST", "SET BW=7.8",
                                       mode, getrssi);
    expect_int("airtime: BW7.8 at SF12 rejected", st == CONFIG_APPLY_REJECTED_INVALID, 1);
    expect_int("airtime: no hardware touched", radio.lora_apply_count, 0);

    /* Same BW with SF7 in the SAME command: ~8.8 s — accepted. */
    st = parse_and_apply_config_generic(radio, "TEST", "SET SF=7 BW=7.8",
                                        mode, getrssi);
    expect_int("airtime: SF7+BW7.8 accepted", st == CONFIG_APPLY_APPLIED, 1);
    expect_int("airtime: both keys applied", radio.lora_apply_count, 2);

    /* Shadow remembers BW7.8: raising SF back to 12 must now be rejected. */
    st = parse_and_apply_config_generic(radio, "TEST", "SET SF=12",
                                        mode, getrssi);
    expect_int("airtime: SF12 on BW7.8 shadow rejected",
               st == CONFIG_APPLY_REJECTED_INVALID, 1);
    expect_int("airtime: rejected key not applied", radio.lora_apply_count, 2);

    /* MODE resets the shadow to band defaults: SF12 is fine again. */
    st = parse_and_apply_config_generic(radio, "TEST", "SET MODE=LORA SF=12",
                                        mode, getrssi);
    expect_int("airtime: MODE reset re-admits SF12",
               st == CONFIG_APPLY_APPLIED, 1);

    config_apply_effective_reset();
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

    /* Baseline band for all MODE-applying tests (config_apply reads the
     * descriptor for the boot RF defaults). */
    daemon_band_resolve(RADIO_BAND_433);

    test_invalid_getrssi_has_no_side_effects();
    test_invalid_fsk_value_has_no_mode_or_getrssi_side_effects();
    test_malformed_token_has_no_side_effects();
    test_failed_mode_switch_aborts_remaining_apply();
    test_valid_mode_and_getrssi_still_apply();
    test_valid_lora_parameter_still_applies();
    test_fsk_rxbw_follows_driver_family();
    test_mode_switch_passes_band_defaults();
    test_mode_switch_applies_before_explicit_params();

    test_airtime_gate_merged_config();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
