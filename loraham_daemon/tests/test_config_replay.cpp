/*
 * The replay record (config_apply.h): what a Meshstick re-init replays after a
 * replug. Through the REAL parse_and_apply_config_generic() on a fake radio:
 *
 *   T4  the last value per key; a MODE switch starts the record over; a key
 *       whose apply failed and a rejected command are not recorded; GETRSSI is
 *       not recorded; the replay command applied to a fresh radio reproduces
 *       the same applies and the same record.
 *
 * GET RADIO (1.5.0, config_radio.h), the formatter on that record:
 *   T1  every band's defaults: FREQ=433.9 (not 433.899994), LDRO=1 on 433,
 *       AUTO on 868/915
 *   T2  each key after a SET, in the SET grammar (SYNC=18 -> 0x12, LDRO=auto
 *       -> AUTO, BW=62.50 -> 62.5, FREQ=+433.175 -> 433.175)
 *   T3  MODE=FSK: LoRa keys -, FREQ/POWER the band's, FSK PREAMBLE/SYNC -
 *       until a successful SET, then the value; MODE=LORA: the defaults again
 *   T4  a refused command and an ignored key change nothing; a driver error
 *       in the middle keeps the keys applied before it (with RADIO=FAILED)
 */

#include "../config_apply.h"
#include "../daemon_band.h"
#include "../radio_driver.h"
#include "../config_policy.h"
#include "../config_radio.h"

#include <atomic>
#include <cstdio>
#include <string>

static int g_ok = 0;
static int g_fail = 0;

static void expect_str(const char *name, const std::string &got, const std::string &want)
{
    if (got == want) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected '%s', got '%s'\n", name, want.c_str(), got.c_str());
    }
}

/* A recorded value as a string; "(none)" when the key has none. */
static std::string recorded(const char *key)
{
    const char *v = config_apply_replay_value(key);
    return v ? v : "(none)";
}

static void expect_int(const char *name, long got, long want)
{
    if (got == want) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %ld, got %ld\n", name, want, got);
    }
}

struct FakeRadio : public RadioDriver {
    int switches = 0, lora_applies = 0, fsk_applies = 0;
    std::string fail_key;

    FakeRadio() : RadioDriver(NULL) {}
    int16_t switchMode(RadioMode_t, const RadioRfDefaults *) override { switches++; return 0; }
    int16_t applyLoraParam(const char *, const std::string &key, const std::string &) override
    {
        lora_applies++;
        return key == fail_key ? -1 : 0;
    }
    int16_t applyFskParam(const char *, const std::string &, const std::string &) override
    {
        fsk_applies++;
        return 0;
    }
    int16_t begin(const RadioRfDefaults *) override { return 0; }
    int16_t clearIrq(uint32_t) override { return 0; }
    float readLiveRssi(RadioMode_t, bool) override { return -200.0f; }
    float rssiProbe() override { return -200.0f; }
    int16_t standby() override { return 0; }
    bool rxDonePending() override { return false; }
    const char *chipName() const override { return "FAKE"; }
    DaemonChipFamily chipFamily() const override { return DAEMON_CHIP_FAMILY_SX1262; }
};

static ConfigApplyStatus apply(FakeRadio &r, const char *cmd, RadioMode_t &mode)
{
    std::atomic<bool> getrssi{false};
    return parse_and_apply_config_generic(r, "868", cmd, mode, getrssi, false);
}

static std::string radio_line(RadioHealth h, RadioMode_t mode)
{
    char buf[CONFIG_RADIO_LINE_MAX];
    config_radio_format(buf, sizeof(buf), h, mode, daemon_band()->rf_defaults);
    return buf;
}

static ConfigApplyStatus apply_on(FakeRadio &r, const char *tag, const char *cmd, RadioMode_t &mode)
{
    std::atomic<bool> getrssi{false};
    return parse_and_apply_config_generic(r, tag, cmd, mode, getrssi, false);
}

static void test_get_radio(void)
{
    /* T1: a fresh record per band: every value is the band's default. */
    const RadioBand_t bands[3] = { RADIO_BAND_433, RADIO_BAND_868, RADIO_BAND_915 };
    const char *want[3] = {
        "RADIO RADIO=READY MODE=LORA FREQ=433.9 SF=12 BW=125.0 CR=5 PREAMBLE=8 SYNC=0x12 CRC=1 LDRO=1 POWER=10\n",
        "RADIO RADIO=READY MODE=LORA FREQ=869.525 SF=11 BW=250.0 CR=5 PREAMBLE=16 SYNC=0x2B CRC=1 LDRO=AUTO POWER=10\n",
        "RADIO RADIO=READY MODE=LORA FREQ=906.875 SF=11 BW=250.0 CR=5 PREAMBLE=16 SYNC=0x2B CRC=1 LDRO=AUTO POWER=10\n",
    };
    for (int i = 0; i < 3; i++) {
        daemon_band_resolve(bands[i]);
        config_apply_replay_reset();
        expect_str("T1 GET RADIO: the band's defaults (433.9, LDRO 1 on 433 / AUTO elsewhere)",
                   radio_line(RADIO_HEALTH_READY, RADIO_MODE_LORA), want[i]);
    }

    /* T2: every key after a successful SET, normalized. */
    daemon_band_resolve(RADIO_BAND_433);
    config_apply_replay_reset();
    config_apply_effective_reset();
    FakeRadio r;
    RadioMode_t mode = RADIO_MODE_LORA;
    expect_int("T2 the SET applies",
               (long)apply_on(r, "433", "SET SF=7 SYNC=18 LDRO=auto BW=62.50 FREQ=+433.175 CR=7 PREAMBLE=12 "
                                        "CRC=0 POWER=5", mode), (long)CONFIG_APPLY_APPLIED);
    const std::string t2 =
        "RADIO RADIO=READY MODE=LORA FREQ=433.175 SF=7 BW=62.5 CR=7 PREAMBLE=12 SYNC=0x12 CRC=0 LDRO=AUTO POWER=5\n";
    expect_str("T2 each key as applied, in the SET grammar", radio_line(RADIO_HEALTH_READY, mode), t2);
    apply_on(r, "433", "SET SYNC=0x34 LDRO=1", mode);
    expect_str("T2 SYNC given in hex, LDRO forced",
               radio_line(RADIO_HEALTH_READY, mode),
               "RADIO RADIO=READY MODE=LORA FREQ=433.175 SF=7 BW=62.5 CR=7 PREAMBLE=12 SYNC=0x34 CRC=0 LDRO=1 POWER=5\n");

    /* T4: a refused command, an ignored key, a driver error in the middle. */
    const std::string before = radio_line(RADIO_HEALTH_READY, mode);
    expect_int("T4 SF=99 is refused", (long)apply_on(r, "433", "SET SF=99", mode),
               (long)CONFIG_APPLY_REJECTED_INVALID);
    expect_str("T4 ... the reply unchanged", radio_line(RADIO_HEALTH_READY, mode), before);
    r.fail_key = "BW";
    apply_on(r, "433", "SET SF=8 BW=125 CR=6", mode);
    r.fail_key.clear();
    expect_str("T4 a driver error at BW: SF=8 (before it) kept, BW and CR as before, RADIO=FAILED",
               radio_line(RADIO_HEALTH_FAILED, mode),
               "RADIO RADIO=FAILED MODE=LORA FREQ=433.175 SF=8 BW=62.5 CR=7 PREAMBLE=12 SYNC=0x34 CRC=0 LDRO=1 POWER=5\n");

    /* T3: FSK. */
    expect_int("T3 MODE=FSK applies", (long)apply_on(r, "433", "SET MODE=FSK", mode), (long)CONFIG_APPLY_APPLIED);
    const std::string fsk =
        "RADIO RADIO=READY MODE=FSK FREQ=433.9 SF=- BW=- CR=- PREAMBLE=- SYNC=- CRC=- LDRO=- POWER=10\n";
    expect_str("T3 FSK: LoRa keys -, FREQ/POWER the band's, FSK PREAMBLE/SYNC - before a SET",
               radio_line(RADIO_HEALTH_READY, mode), fsk);
    apply_on(r, "433", "SET SF=9", mode);
    expect_str("T4 a LoRa key in FSK is ignored: the reply unchanged", radio_line(RADIO_HEALTH_READY, mode), fsk);
    expect_int("T3 FSK PREAMBLE and SYNC set", (long)apply_on(r, "433", "SET PREAMBLE=32 SYNC=0x2DD4", mode),
               (long)CONFIG_APPLY_APPLIED);
    expect_str("T3 FSK: PREAMBLE and SYNC as applied",
               radio_line(RADIO_HEALTH_READY, mode),
               "RADIO RADIO=READY MODE=FSK FREQ=433.9 SF=- BW=- CR=- PREAMBLE=32 SYNC=0x2DD4 CRC=- LDRO=- POWER=10\n");
    apply_on(r, "433", "SET MODE=LORA", mode);
    expect_str("T3 MODE=LORA after the SETs: the band's defaults again",
               radio_line(RADIO_HEALTH_READY, mode), want[0]);
    config_apply_replay_reset();
    config_apply_effective_reset();
}

int main(void)
{
    daemon_band_resolve(RADIO_BAND_868);
    config_apply_replay_reset();
    config_apply_effective_reset();
    FakeRadio r;
    RadioMode_t mode = RADIO_MODE_LORA;

    expect_str("nothing applied: no replay", config_apply_replay_command(), "");

    apply(r, "SET SF=9 BW=125", mode);
    expect_str("keys recorded in order", config_apply_replay_command(), "SET SF=9 BW=125");
    /* STATUS FREQ=/SF=/POWER= read these values (config_replay.h). */
    expect_str("the applied SF as written", recorded("SF"), "9");
    expect_str("a key never applied has no value", recorded("FREQ"), "(none)");
    apply(r, "SET SF=10 POWER=5", mode);
    expect_str("last value per key, new keys appended", config_apply_replay_command(),
               "SET SF=10 BW=125 POWER=5");

    apply(r, "SET GETRSSI=1", mode);
    expect_str("GETRSSI is not recorded", config_apply_replay_command(), "SET SF=10 BW=125 POWER=5");

    apply(r, "SET SF=99", mode);
    expect_str("a rejected command is not recorded", config_apply_replay_command(),
               "SET SF=10 BW=125 POWER=5");
    expect_str("a rejected value leaves the applied SF", recorded("SF"), "10");
    expect_str("the applied POWER", recorded("POWER"), "5");

    r.fail_key = "BW";
    apply(r, "SET SF=11 BW=250", mode);
    r.fail_key.clear();
    expect_str("a key whose apply failed is not recorded (the one before it is)",
               config_apply_replay_command(), "SET SF=11 BW=125 POWER=5");

    apply(r, "SET MODE=FSK BR=9.6", mode);
    expect_str("a MODE switch starts the record over", config_apply_replay_command(),
               "SET MODE=FSK BR=9.6");
    expect_str("a MODE switch clears the applied SF", recorded("SF"), "(none)");
    expect_str("a MODE switch clears the applied POWER", recorded("POWER"), "(none)");

    apply(r, "SET MODE=LORA SF=8", mode);
    std::string replay = config_apply_replay_command();
    expect_str("back to LoRa", replay, "SET MODE=LORA SF=8");

    /* The replay on a fresh radio (a re-init: boot defaults, LoRa). */
    FakeRadio fresh;
    RadioMode_t fresh_mode = RADIO_MODE_LORA;
    config_apply_effective_reset();
    expect_int("the replay applies", (long)apply(fresh, replay.c_str(), fresh_mode),
               (long)CONFIG_APPLY_APPLIED);
    expect_int("the replay switches the mode once", fresh.switches, 1);
    expect_int("the replay applies the recorded key", fresh.lora_applies, 1);
    expect_str("the record after the replay equals the record before", config_apply_replay_command(),
               replay);

    test_get_radio();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
