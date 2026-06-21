#include "../daemon_tx_mode_boot.h"

#include <stdio.h>

/* --- Boot TX mode CLI parse + precedence tests -------------------------- */

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

static void test_parse_values(void)
{
    DaemonTxModeBoot mode = DAEMON_TX_MODE_BOOT_UNSET;

    expect_int("parse direct ok", daemon_parse_tx_mode_boot("direct", &mode), 1);
    expect_int("parse direct value", mode, DAEMON_TX_MODE_BOOT_DIRECT);

    expect_int("parse managed ok", daemon_parse_tx_mode_boot("managed", &mode), 1);
    expect_int("parse managed value", mode, DAEMON_TX_MODE_BOOT_MANAGED);

    // Case-insensitive.
    expect_int("parse DIRECT upper ok", daemon_parse_tx_mode_boot("DIRECT", &mode), 1);
    expect_int("parse DIRECT upper value", mode, DAEMON_TX_MODE_BOOT_DIRECT);
    expect_int("parse Managed mixed ok", daemon_parse_tx_mode_boot("Managed", &mode), 1);
    expect_int("parse Managed mixed value", mode, DAEMON_TX_MODE_BOOT_MANAGED);

    // Invalid values fail.
    expect_int("parse invalid fails", daemon_parse_tx_mode_boot("raw", &mode), 0);
    expect_int("parse empty fails", daemon_parse_tx_mode_boot("", &mode), 0);
    expect_int("parse null fails", daemon_parse_tx_mode_boot(NULL, &mode), 0);
}

static void test_global_applies_to_both(void)
{
    daemon_tx_mode_boot_reset();

    expect_int("global direct set ok", daemon_set_tx_mode_boot_global("direct"), 1);
    expect_int("global direct 433", daemon_tx_mode_boot_effective_433(),
               DAEMON_TX_MODE_BOOT_DIRECT);
    expect_int("global direct 868", daemon_tx_mode_boot_effective_868(),
               DAEMON_TX_MODE_BOOT_DIRECT);
}

static void test_default_is_managed(void)
{
    daemon_tx_mode_boot_reset();

    // No flags: both default MANAGED.
    expect_int("default 433 managed", daemon_tx_mode_boot_effective_433(),
               DAEMON_TX_MODE_BOOT_MANAGED);
    expect_int("default 868 managed", daemon_tx_mode_boot_effective_868(),
               DAEMON_TX_MODE_BOOT_MANAGED);
}

static void test_per_band_overrides_global(void)
{
    daemon_tx_mode_boot_reset();

    // --tx-mode=managed --tx-mode-868=direct
    expect_int("set global managed", daemon_set_tx_mode_boot_global("managed"), 1);
    expect_int("set 868 direct", daemon_set_tx_mode_boot_868("direct"), 1);
    expect_int("override 433 managed", daemon_tx_mode_boot_effective_433(),
               DAEMON_TX_MODE_BOOT_MANAGED);
    expect_int("override 868 direct", daemon_tx_mode_boot_effective_868(),
               DAEMON_TX_MODE_BOOT_DIRECT);
}

static void test_precedence_is_order_independent(void)
{
    daemon_tx_mode_boot_reset();

    // Reversed order: --tx-mode-868=direct --tx-mode=managed
    expect_int("set 868 direct first", daemon_set_tx_mode_boot_868("direct"), 1);
    expect_int("set global managed second", daemon_set_tx_mode_boot_global("managed"), 1);
    expect_int("reversed 433 managed", daemon_tx_mode_boot_effective_433(),
               DAEMON_TX_MODE_BOOT_MANAGED);
    expect_int("reversed 868 direct", daemon_tx_mode_boot_effective_868(),
               DAEMON_TX_MODE_BOOT_DIRECT);
}

static void test_per_band_only(void)
{
    daemon_tx_mode_boot_reset();

    // --tx-mode-433=direct only: 433 DIRECT, 868 default MANAGED.
    expect_int("set 433 direct only", daemon_set_tx_mode_boot_433("direct"), 1);
    expect_int("per-band 433 direct", daemon_tx_mode_boot_effective_433(),
               DAEMON_TX_MODE_BOOT_DIRECT);
    expect_int("per-band 868 default managed", daemon_tx_mode_boot_effective_868(),
               DAEMON_TX_MODE_BOOT_MANAGED);
}

static void test_invalid_setter_fails(void)
{
    daemon_tx_mode_boot_reset();

    expect_int("invalid global setter fails",
               daemon_set_tx_mode_boot_global("bogus"), 0);
    // State unchanged → still default MANAGED.
    expect_int("invalid leaves 433 default", daemon_tx_mode_boot_effective_433(),
               DAEMON_TX_MODE_BOOT_MANAGED);
}

int main(void)
{
    test_parse_values();
    test_global_applies_to_both();
    test_default_is_managed();
    test_per_band_overrides_global();
    test_precedence_is_order_independent();
    test_per_band_only();
    test_invalid_setter_fails();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
