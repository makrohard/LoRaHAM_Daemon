/*
 * T4 on the real radio path: the replay record applied after a re-init, with
 * the REAL Sx1262Driver and the REAL pinned RadioLib over
 * tests/fakes/sx126x_register_model.h, through the REAL
 * parse_and_apply_config_generic() (what op_reinit() calls after lora_init()).
 *
 *   - every LoRa key (SF BW FREQ CR CRC PREAMBLE SYNC LDRO POWER) and every
 *     FSK key the SX1262 accepts (FREQ POWER BR FREQDEV RXBW SHAPING ENCODING
 *     PREAMBLE SYNC; OOK is refused on this chip and never recorded) enters
 *     the record, last value per key, and a MODE switch starts it over;
 *   - POWER=22 is applied on replay with the 21/22 dBm warning printed again;
 *   - the over-current limit (RegOcp) is written after the re-init's begin()
 *     and again after a replayed MODE switch: the final value is the
 *     configured one, never RadioLib's 60 mA;
 *   - a rejected value is not recorded and not replayed.
 */

#include "../config_apply.h"
#include "../config_policy.h"
#include "../daemon_band.h"
#include "../sx1262_driver.h"
#include "fakes/sx126x_register_model.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

static int g_ok = 0;
static int g_fail = 0;

static void expect(const std::string &name, bool cond)
{
    if (cond) {
        g_ok++;
        printf("[ OK ] %s\n", name.c_str());
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name.c_str());
    }
}

static const uint16_t REG_OCP = 0x08E7;
static const uint8_t OCP_60MA = 60 / 2.5;
static const uint8_t OCP_140MA = 140 / 2.5;

struct Rig {
    Sx126xRegisterModel model;
    Module mod;
    Sx1262Driver drv;
    Rig() : mod(&model, 21, 16, 18, 20), drv(&mod, 1.8f, -1, 140) { model.reset(); }
};

static RadioRfDefaults defaults(void)
{
    RadioRfDefaults def{};
    def.freq_mhz = 869.525f;
    def.spreading_factor = 11;
    def.bandwidth_khz = 250.0f;
    def.sync_word = 0x2B;
    def.preamble_len = 16;
    def.coding_rate = 5;
    def.crc_on = true;
    def.ldro = -1;
    def.power_dbm = 10;
    return def;
}

/* Apply `cmd` with stdout captured; returns the status, *out the text. */
static ConfigApplyStatus apply(Rig &rig, const char *cmd, RadioMode_t &mode, std::string *out)
{
    std::atomic<bool> getrssi{false};
    char path[] = "/tmp/loraham-replay-out.XXXXXX";
    int fd = mkstemp(path);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    ConfigApplyStatus st = parse_and_apply_config_generic(rig.drv, "868", cmd, mode, getrssi, true);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    out->clear();
    char buf[4096];
    lseek(fd, 0, SEEK_SET);
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        out->append(buf, (size_t)n);
    close(fd);
    unlink(path);
    return st;
}

static bool has_key(const std::string &cmd, const char *key)
{
    return cmd.find(std::string(" ") + key + "=") != std::string::npos;
}

int main(void)
{
    daemon_band_resolve(RADIO_BAND_868);
    config_policy_set_sx1262_power_max(22);      /* what the meshstick-1262 preset sets at startup */
    config_apply_replay_reset();
    config_apply_effective_reset();
    RadioRfDefaults def = defaults();
    std::string out;

    /* The radio as the clients configured it. */
    Rig first;
    RadioMode_t mode = RADIO_MODE_LORA;
    expect("boot", first.drv.begin(&def) == RADIOLIB_ERR_NONE);
    expect("every LoRa key applied",
           apply(first, "SET SF=9 BW=125 FREQ=869.4 CR=6 CRC=0 PREAMBLE=12 SYNC=0x12 LDRO=1 POWER=14",
                 mode, &out) == CONFIG_APPLY_APPLIED);
    expect("a rejected value is refused", apply(first, "SET SF=99", mode, &out) != CONFIG_APPLY_APPLIED);
    expect("POWER=22 applied", apply(first, "SET POWER=22", mode, &out) == CONFIG_APPLY_APPLIED);
    if (out.find("WARNING POWER=22") == std::string::npos)
        printf("       apply output: %s\n", out.c_str());
    expect("POWER=22 warns", out.find("WARNING POWER=22") != std::string::npos);
    std::string lora = config_apply_replay_command();
    static const char *lora_keys[] = { "SF", "BW", "FREQ", "CR", "CRC", "PREAMBLE", "SYNC", "LDRO", "POWER" };
    bool all = true;
    for (const char *k : lora_keys)
        all = all && has_key(lora, k);
    expect("the record holds every LoRa key: " + lora, all);
    expect("last value per key (POWER=22, not 14)", lora.find("POWER=22") != std::string::npos &&
           lora.find("POWER=14") == std::string::npos);
    expect("the rejected SF=99 is not recorded", lora.find("SF=99") == std::string::npos);

    /* Re-init on a fresh chip (a replugged stick): boot defaults, then replay. */
    Rig again;
    RadioMode_t mode2 = RADIO_MODE_LORA;
    config_apply_effective_reset();
    expect("re-init begin()", again.drv.begin(&def) == RADIOLIB_ERR_NONE);
    expect("re-init: OCP written after begin() (140 mA, not RadioLib's 60)",
           again.model.peek(REG_OCP) == OCP_140MA);
    expect("LoRa replay applies", apply(again, lora.c_str(), mode2, &out) == CONFIG_APPLY_APPLIED);
    expect("LoRa replay: the 22 dBm warning again", out.find("WARNING POWER=22") != std::string::npos);
    expect("LoRa replay: the record is unchanged", config_apply_replay_command() == lora);
    expect("LoRa replay: OCP still 140 mA", again.model.peek(REG_OCP) == OCP_140MA);

    /* FSK: every key; the replay switches the mode, which re-runs beginFSK(). */
    expect("MODE=FSK with every FSK key applied",
           apply(again, "SET MODE=FSK FREQ=869.5 POWER=22 BR=9.6 FREQDEV=5 RXBW=58.6 SHAPING=0.5 "
                        "ENCODING=0 PREAMBLE=24 SYNC=0x2D01", mode2, &out) == CONFIG_APPLY_APPLIED);
    expect("OOK is refused on the SX1262 (no OOK modulator) and so never recorded",
           apply(again, "SET OOK=0", mode2, &out) != CONFIG_APPLY_APPLIED);
    std::string fsk = config_apply_replay_command();
    static const char *fsk_keys[] = { "FREQ", "POWER", "BR", "FREQDEV", "RXBW", "SHAPING",
                                      "ENCODING", "PREAMBLE", "SYNC" };
    all = fsk.find("MODE=FSK") != std::string::npos;
    for (const char *k : fsk_keys)
        all = all && has_key(fsk, k);
    expect("the FSK record holds MODE and every FSK key: " + fsk, all);
    expect("the MODE switch started the record over (no LoRa-only key)",
           !has_key(fsk, "SF") && !has_key(fsk, "BW") && !has_key(fsk, "LDRO"));

    Rig third;
    RadioMode_t mode3 = RADIO_MODE_LORA;
    config_apply_effective_reset();
    expect("re-init #2 begin()", third.drv.begin(&def) == RADIOLIB_ERR_NONE);
    third.model.poke(REG_OCP, OCP_60MA);          /* make the replayed MODE's own write visible */
    expect("FSK replay applies", apply(third, fsk.c_str(), mode3, &out) == CONFIG_APPLY_APPLIED);
    expect("FSK replay: the mode switched", mode3 == RADIO_MODE_FSK);
    expect("FSK replay: setCurrentLimit after the replayed MODE (OCP 140 mA again)",
           third.model.peek(REG_OCP) == OCP_140MA);
    expect("FSK replay: the 22 dBm warning again", out.find("WARNING POWER=22") != std::string::npos);
    expect("FSK replay: the record is unchanged", config_apply_replay_command() == fsk);

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
