/*
 * The replay record (config_apply.h): what a Meshstick re-init replays after a
 * replug. Through the REAL parse_and_apply_config_generic() on a fake radio:
 *
 *   T4  the last value per key; a MODE switch starts the record over; a key
 *       whose apply failed and a rejected command are not recorded; GETRSSI is
 *       not recorded; the replay command applied to a fresh radio reproduces
 *       the same applies and the same record.
 */

#include "../config_apply.h"
#include "../daemon_band.h"
#include "../radio_driver.h"
#include "../config_policy.h"

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

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
