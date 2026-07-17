#include "../config_validate.h"

#include "../config_parser.h"
#include "../radio_channel.h"

#include <stdio.h>
#include <string.h>

/* --- Transactional CONFIG validation tests --- */

static int g_ok = 0;
static int g_fail = 0;

static void expect_int(const char *name, int actual, int expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %d, got %d\n", name, expected, actual);
    }
}

static void expect_str(const char *name,
                       const std::string &actual,
                       const char *expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %s, got %s\n",
               name, expected, actual.c_str());
    }
}

static ConfigValidationResult validate(const char *cmd,
                                       RadioMode_t current_mode,
                                       int *ok)
{
    ConfigCommand parsed = config_parse_command(cmd);
    ConfigValidationResult result;

    /* 433-band policy: tests validate against the 430–440 MHz range. */
    *ok = config_validate_command(parsed, current_mode, &result,
                                  DAEMON_CHIP_FAMILY_SX127X,
                                  430.0f, 440.0f);

    return result;
}

static ConfigValidationResult validate_family(const char *cmd,
                                              RadioMode_t current_mode,
                                              DaemonChipFamily family,
                                              int *ok)
{
    ConfigCommand parsed = config_parse_command(cmd);
    ConfigValidationResult result;

    *ok = config_validate_command(parsed, current_mode, &result, family,
                                  430.0f, 440.0f);

    return result;
}

static void test_valid_lora_command(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET GETRSSI=1 SF=12 BW=125 FREQ=433.900",
                 RADIO_MODE_LORA, &ok);

    expect_int("valid lora command", ok, 1);
    expect_int("valid lora result flag", result.valid, 1);
    expect_int("valid lora target", result.target_mode, RADIO_MODE_LORA);
}

static void test_invalid_lora_value_rejects_whole_command(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET GETRSSI=1 SF=12 FREQ=433abc",
                 RADIO_MODE_LORA, &ok);

    expect_int("invalid lora command rejected", ok, 0);
    expect_int("invalid lora result flag", result.valid, 0);
    expect_str("invalid lora key", result.key, "FREQ");
}

static void test_invalid_getrssi_rejects_whole_command(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET GETRSSI=2 SF=12",
                 RADIO_MODE_LORA, &ok);

    expect_int("invalid getrssi rejected", ok, 0);
    expect_str("invalid getrssi key", result.key, "GETRSSI");
}

static void test_valid_fsk_target_mode(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET MODE=FSK BR=1.2 RXBW=12.5 SYNC=0x2DD4",
                 RADIO_MODE_LORA, &ok);

    expect_int("valid fsk command", ok, 1);
    expect_int("valid fsk target", result.target_mode, RADIO_MODE_FSK);
}

static void test_fsk_rxbw_family_raster(void)
{
    int ok = 0;

    // SX1262 family: chip raster accepted, SX127x raster rejected.
    validate_family("SET MODE=FSK BR=9.6 RXBW=23.4",
                    RADIO_MODE_LORA, DAEMON_CHIP_FAMILY_SX1262, &ok);
    expect_int("sx1262 rxbw 23.4 accepted", ok, 1);

    ConfigValidationResult result =
        validate_family("SET MODE=FSK BR=9.6 RXBW=25.0",
                        RADIO_MODE_LORA, DAEMON_CHIP_FAMILY_SX1262, &ok);
    expect_int("sx1262 rxbw 25.0 rejected", ok, 0);
    expect_str("sx1262 rxbw reject key", result.key, "RXBW");

    // SX127x family: unchanged semantics.
    validate_family("SET MODE=FSK BR=9.6 RXBW=23.4",
                    RADIO_MODE_LORA, DAEMON_CHIP_FAMILY_SX127X, &ok);
    expect_int("sx127x rxbw 23.4 rejected", ok, 0);

    // Default parameter = legacy SX127x semantics for driverless callers.
    validate("SET MODE=FSK BR=9.6 RXBW=25.0", RADIO_MODE_LORA, &ok);
    expect_int("default family rxbw 25.0 accepted", ok, 1);
}

static void test_invalid_fsk_value_rejects_before_mode_switch(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET MODE=FSK BR=bad RXBW=12.5",
                 RADIO_MODE_LORA, &ok);

    expect_int("invalid fsk command rejected", ok, 0);
    expect_int("invalid fsk target still reported", result.target_mode, RADIO_MODE_FSK);
    expect_str("invalid fsk key", result.key, "BR");
}

static void test_invalid_mode_rejects_whole_command(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET MODE=BAD SF=12",
                 RADIO_MODE_LORA, &ok);

    expect_int("invalid mode rejected", ok, 0);
    expect_str("invalid mode key", result.key, "MODE");
}

static void test_ignored_wrong_mode_keys_preserve_compatibility(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET BR=bad RXBW=also_bad",
                 RADIO_MODE_LORA, &ok);

    expect_int("fsk keys ignored in lora mode stay accepted", ok, 1);
    expect_int("ignored fsk target", result.target_mode, RADIO_MODE_LORA);

    result = validate("SET SF=bad LDRO=bad",
                      RADIO_MODE_FSK, &ok);

    expect_int("lora keys ignored in fsk mode stay accepted", ok, 1);
    expect_int("ignored lora target", result.target_mode, RADIO_MODE_FSK);
}

static void test_unknown_key_rejects_whole_command(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET POWR=10 SF=12",
                 RADIO_MODE_LORA, &ok);

    expect_int("unknown key rejected", ok, 0);
    expect_int("unknown key result flag", result.valid, 0);
    expect_str("unknown key name", result.key, "POWR");
    expect_str("unknown key reason", result.reason, "unknown key");
}


static void test_malformed_token_rejects_whole_command(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET GETRSSI=1 SF=12 BROKEN",
                 RADIO_MODE_LORA, &ok);

    expect_int("malformed token rejected", ok, 0);
    expect_int("malformed result flag", result.valid, 0);
    expect_str("malformed token key", result.key, "BROKEN");
}


/* Audit P1-4: FREQ outside the band descriptor's range fails closed — an
 * 868 value must never validate under the 433-band policy. */
static void test_freq_outside_band_rejected(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET FREQ=869.525", RADIO_MODE_LORA, &ok);

    expect_int("off-band freq rejected", ok, 0);
    expect_str("off-band freq key", result.key, "FREQ");

    validate("SET FREQ=433.900", RADIO_MODE_LORA, &ok);
    expect_int("in-band freq accepted", ok, 1);

    validate("SET FREQ=429.999", RADIO_MODE_LORA, &ok);
    expect_int("below-band freq rejected", ok, 0);
}

/* Audit P2-1: duplicate keys are ambiguous (last-one-wins under sequential
 * apply) and fail closed. */
static void test_duplicate_key_rejected(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET SF=12 SF=7", RADIO_MODE_LORA, &ok);

    expect_int("duplicate key rejected", ok, 0);
    expect_str("duplicate key name", result.key, "SF");
    expect_str("duplicate key reason", result.reason, "duplicate key");

    validate("SET SF=12 BW=125", RADIO_MODE_LORA, &ok);
    expect_int("distinct keys accepted", ok, 1);
}

/* Audit P1-3: MODE is extracted from the token list — duplicates must fail
 * closed like every other duplicate key, and SX1262 capability gaps reject
 * during prevalidation (before any mode switch side effect). */
static void test_duplicate_mode_rejected(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate("SET MODE=FSK MODE=LORA", RADIO_MODE_LORA, &ok);

    expect_int("duplicate MODE rejected", ok, 0);
    expect_str("duplicate MODE reason", result.reason, "duplicate key");
}

static void test_sx1262_capabilities_prevalidated(void)
{
    int ok = 0;
    ConfigValidationResult result =
        validate_family("SET MODE=FSK OOK=1", RADIO_MODE_LORA,
                        DAEMON_CHIP_FAMILY_SX1262, &ok);

    expect_int("sx1262 OOK=1 prevalidation reject", ok, 0);
    expect_str("sx1262 OOK key", result.key, "OOK");

    validate_family("SET MODE=FSK ENCODING=1", RADIO_MODE_LORA,
                    DAEMON_CHIP_FAMILY_SX1262, &ok);
    expect_int("sx1262 ENCODING=1 prevalidation reject", ok, 0);

    validate_family("SET MODE=FSK FREQDEV=0.1", RADIO_MODE_LORA,
                    DAEMON_CHIP_FAMILY_SX1262, &ok);
    expect_int("sx1262 FREQDEV=0.1 prevalidation reject", ok, 0);

    validate_family("SET MODE=FSK OOK=1", RADIO_MODE_LORA,
                    DAEMON_CHIP_FAMILY_SX127X, &ok);
    expect_int("sx127x OOK=1 still accepted", ok, 1);
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

    test_valid_lora_command();
    test_invalid_lora_value_rejects_whole_command();
    test_invalid_getrssi_rejects_whole_command();
    test_valid_fsk_target_mode();
    test_fsk_rxbw_family_raster();
    test_invalid_fsk_value_rejects_before_mode_switch();
    test_invalid_mode_rejects_whole_command();
    test_ignored_wrong_mode_keys_preserve_compatibility();
    test_unknown_key_rejects_whole_command();
    test_malformed_token_rejects_whole_command();

    test_freq_outside_band_rejected();
    test_duplicate_key_rejected();
    test_duplicate_mode_rejected();
    test_sx1262_capabilities_prevalidated();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
