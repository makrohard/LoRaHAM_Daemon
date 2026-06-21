#include "../daemon_cad_monitor_boot.h"

#include <stdio.h>

/* --- Boot CAD monitor CLI parse + precedence tests ---------------------- */

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
    DaemonCadMonitorBoot v = DAEMON_CAD_MONITOR_BOOT_UNSET;

    expect_int("parse on ok", daemon_parse_cad_monitor_boot("on", &v), 1);
    expect_int("parse on value", v, DAEMON_CAD_MONITOR_BOOT_ON);

    expect_int("parse off ok", daemon_parse_cad_monitor_boot("off", &v), 1);
    expect_int("parse off value", v, DAEMON_CAD_MONITOR_BOOT_OFF);

    // Case-insensitive.
    expect_int("parse ON upper ok", daemon_parse_cad_monitor_boot("ON", &v), 1);
    expect_int("parse ON upper value", v, DAEMON_CAD_MONITOR_BOOT_ON);
    expect_int("parse Off mixed ok", daemon_parse_cad_monitor_boot("Off", &v), 1);
    expect_int("parse Off mixed value", v, DAEMON_CAD_MONITOR_BOOT_OFF);

    // Invalid values fail.
    expect_int("parse invalid fails", daemon_parse_cad_monitor_boot("1", &v), 0);
    expect_int("parse empty fails", daemon_parse_cad_monitor_boot("", &v), 0);
    expect_int("parse null fails", daemon_parse_cad_monitor_boot(NULL, &v), 0);
}

static void test_default_is_off(void)
{
    daemon_cad_monitor_boot_reset();

    expect_int("default 433 off", daemon_cad_monitor_boot_effective_433() ? 1 : 0, 0);
    expect_int("default 868 off", daemon_cad_monitor_boot_effective_868() ? 1 : 0, 0);
}

static void test_global_applies_to_both(void)
{
    daemon_cad_monitor_boot_reset();

    expect_int("global on set ok", daemon_set_cad_monitor_boot_global("on"), 1);
    expect_int("global on 433", daemon_cad_monitor_boot_effective_433() ? 1 : 0, 1);
    expect_int("global on 868", daemon_cad_monitor_boot_effective_868() ? 1 : 0, 1);
}

static void test_per_band_overrides_global(void)
{
    daemon_cad_monitor_boot_reset();

    // --cad-monitor=on --cad-monitor-868=off
    expect_int("set global on", daemon_set_cad_monitor_boot_global("on"), 1);
    expect_int("set 868 off", daemon_set_cad_monitor_boot_868("off"), 1);
    expect_int("override 433 on", daemon_cad_monitor_boot_effective_433() ? 1 : 0, 1);
    expect_int("override 868 off", daemon_cad_monitor_boot_effective_868() ? 1 : 0, 0);
}

static void test_precedence_is_order_independent(void)
{
    daemon_cad_monitor_boot_reset();

    // Reversed order: --cad-monitor-868=off --cad-monitor=on
    expect_int("set 868 off first", daemon_set_cad_monitor_boot_868("off"), 1);
    expect_int("set global on second", daemon_set_cad_monitor_boot_global("on"), 1);
    expect_int("reversed 433 on", daemon_cad_monitor_boot_effective_433() ? 1 : 0, 1);
    expect_int("reversed 868 off", daemon_cad_monitor_boot_effective_868() ? 1 : 0, 0);
}

static void test_per_band_only(void)
{
    daemon_cad_monitor_boot_reset();

    // --cad-monitor-433=on only: 433 on, 868 default off.
    expect_int("set 433 on only", daemon_set_cad_monitor_boot_433("on"), 1);
    expect_int("per-band 433 on", daemon_cad_monitor_boot_effective_433() ? 1 : 0, 1);
    expect_int("per-band 868 default off", daemon_cad_monitor_boot_effective_868() ? 1 : 0, 0);
}

static void test_invalid_setter_keeps_default(void)
{
    daemon_cad_monitor_boot_reset();

    expect_int("invalid global setter fails",
               daemon_set_cad_monitor_boot_global("bogus"), 0);
    expect_int("invalid leaves 433 default off",
               daemon_cad_monitor_boot_effective_433() ? 1 : 0, 0);
}

int main(void)
{
    test_parse_values();
    test_default_is_off();
    test_global_applies_to_both();
    test_per_band_overrides_global();
    test_precedence_is_order_independent();
    test_per_band_only();
    test_invalid_setter_keeps_default();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
