#include "../daemon_cad_rssi_boot.h"

#include <stdio.h>

/* --- Boot CAD RSSI threshold CLI parse + precedence tests --------------- */

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
    float v = 0.0f;

    expect_int("parse -90 ok", daemon_parse_cad_rssi_boot("-90", &v), 1);
    expect_int("parse -90 value", (int)v, -90);

    expect_int("parse 0 ok", daemon_parse_cad_rssi_boot("0", &v), 1);
    expect_int("parse 0 value", (int)v, 0);

    expect_int("parse -130 ok (low bound)", daemon_parse_cad_rssi_boot("-130", &v), 1);

    // Out of range / invalid.
    expect_int("parse -131 fails (too low)", daemon_parse_cad_rssi_boot("-131", &v), 0);
    expect_int("parse 1 fails (too high)", daemon_parse_cad_rssi_boot("1", &v), 0);
    expect_int("parse junk fails", daemon_parse_cad_rssi_boot("-90x", &v), 0);
    expect_int("parse empty fails", daemon_parse_cad_rssi_boot("", &v), 0);
    expect_int("parse null fails", daemon_parse_cad_rssi_boot(NULL, &v), 0);
}

static void test_default_unset(void)
{
    float v = 0.0f;
    daemon_cad_rssi_boot_reset();

    expect_int("default 433 unset", daemon_cad_rssi_boot_effective_433(&v) ? 1 : 0, 0);
    expect_int("default 868 unset", daemon_cad_rssi_boot_effective_868(&v) ? 1 : 0, 0);
}

static void test_global_applies_to_both(void)
{
    float v433 = 0.0f, v868 = 0.0f;
    daemon_cad_rssi_boot_reset();

    expect_int("global set ok", daemon_set_cad_rssi_boot_global("-85"), 1);
    expect_int("global 433 set", daemon_cad_rssi_boot_effective_433(&v433) ? 1 : 0, 1);
    expect_int("global 433 value", (int)v433, -85);
    expect_int("global 868 set", daemon_cad_rssi_boot_effective_868(&v868) ? 1 : 0, 1);
    expect_int("global 868 value", (int)v868, -85);
}

static void test_per_band_overrides_global(void)
{
    float v433 = 0.0f, v868 = 0.0f;
    daemon_cad_rssi_boot_reset();

    // --cad-rssi=-85 --cad-rssi-868=-100
    expect_int("set global -85", daemon_set_cad_rssi_boot_global("-85"), 1);
    expect_int("set 868 -100", daemon_set_cad_rssi_boot_868("-100"), 1);
    daemon_cad_rssi_boot_effective_433(&v433);
    daemon_cad_rssi_boot_effective_868(&v868);
    expect_int("override 433 = global -85", (int)v433, -85);
    expect_int("override 868 = -100", (int)v868, -100);
}

static void test_precedence_is_order_independent(void)
{
    float v433 = 0.0f, v868 = 0.0f;
    daemon_cad_rssi_boot_reset();

    // Reversed order.
    expect_int("set 868 -100 first", daemon_set_cad_rssi_boot_868("-100"), 1);
    expect_int("set global -85 second", daemon_set_cad_rssi_boot_global("-85"), 1);
    daemon_cad_rssi_boot_effective_433(&v433);
    daemon_cad_rssi_boot_effective_868(&v868);
    expect_int("reversed 433 = -85", (int)v433, -85);
    expect_int("reversed 868 = -100", (int)v868, -100);
}

static void test_per_band_only(void)
{
    float v433 = 0.0f, v868 = 0.0f;
    daemon_cad_rssi_boot_reset();

    expect_int("set 433 -75 only", daemon_set_cad_rssi_boot_433("-75"), 1);
    expect_int("per-band 433 set", daemon_cad_rssi_boot_effective_433(&v433) ? 1 : 0, 1);
    expect_int("per-band 433 value", (int)v433, -75);
    expect_int("per-band 868 unset", daemon_cad_rssi_boot_effective_868(&v868) ? 1 : 0, 0);
}

int main(void)
{
    test_parse_values();
    test_default_unset();
    test_global_applies_to_both();
    test_per_band_overrides_global();
    test_precedence_is_order_independent();
    test_per_band_only();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
