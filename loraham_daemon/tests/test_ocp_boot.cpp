#include "../daemon_ocp_boot.h"

#include <stdio.h>
#include <string.h>

/* --- Boot OCP (--ocp) tests ----------------------------------------------- */
/*
 * The parser takes a strict non-negative integer; the family check accepts
 * exactly the values the chip can represent; the effective value is --ocp or
 * the family default; the startup notice is one line or nothing. The refusal
 * and exit code of the binary itself are in test_interface_baseline.
 */

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

static void expect_str(const char *name, const char *actual, const char *expected)
{
    if (strcmp(actual, expected) == 0) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected \"%s\", got \"%s\"\n", name, expected, actual);
    }
}

static const DaemonChipFamily SX127X = DAEMON_CHIP_FAMILY_SX127X;
static const DaemonChipFamily SX1262 = DAEMON_CHIP_FAMILY_SX1262;

/* ---- parsing ------------------------------------------------------------- */

static void test_parse(void)
{
    static const char *bad[] = { "", "abc", "12x", "-5", "+100", "1.5", " 100", "99999999999" };

    daemon_ocp_boot_reset();
    expect_int("unset -> not given", daemon_ocp_boot_given(), 0);

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char name[64];
        daemon_ocp_boot_reset();
        snprintf(name, sizeof(name), "parse \"%s\" refused", bad[i]);
        expect_int(name, daemon_set_ocp_boot_global(bad[i]), 0);
        expect_int("  ... and stays not given", daemon_ocp_boot_given(), 0);
    }
    daemon_ocp_boot_reset();
    expect_int("parse NULL refused", daemon_set_ocp_boot_global(NULL), 0);

    daemon_ocp_boot_reset();
    expect_int("parse \"150\" accepted", daemon_set_ocp_boot_global("150"), 1);
    expect_int("150 given", daemon_ocp_boot_given(), 1);
    expect_int("150 requested", daemon_ocp_boot_requested_ma(), 150);

    daemon_ocp_boot_reset();
    expect_int("parse \"0\" accepted", daemon_set_ocp_boot_global("0"), 1);
    expect_int("0 requested", daemon_ocp_boot_requested_ma(), 0);
}

/* ---- the representable sets ---------------------------------------------- */

static void test_valid_sets(void)
{
    int sx127x_count = 0, sx1262_count = 0;

    for (int ma = -5; ma <= 300; ma++) {
        if (daemon_ocp_valid_ma(SX127X, ma))
            sx127x_count++;
        if (daemon_ocp_valid_ma(SX1262, ma))
            sx1262_count++;
    }
    /* SX127x: 0, 45..120/5 (16 values), 130..240/10 (12 values). */
    expect_int("SX127x: 29 representable values", sx127x_count, 29);
    /* SX1262: 5..140/5 (28 values). */
    expect_int("SX1262: 28 representable values", sx1262_count, 28);

    expect_int("SX127x 0 (off)", daemon_ocp_valid_ma(SX127X, 0), 1);
    expect_int("SX127x 44", daemon_ocp_valid_ma(SX127X, 44), 0);
    expect_int("SX127x 40", daemon_ocp_valid_ma(SX127X, 40), 0);
    expect_int("SX127x 45", daemon_ocp_valid_ma(SX127X, 45), 1);
    expect_int("SX127x 100", daemon_ocp_valid_ma(SX127X, 100), 1);
    expect_int("SX127x 120", daemon_ocp_valid_ma(SX127X, 120), 1);
    expect_int("SX127x 125 (gap)", daemon_ocp_valid_ma(SX127X, 125), 0);
    expect_int("SX127x 130", daemon_ocp_valid_ma(SX127X, 130), 1);
    expect_int("SX127x 135 (step 10)", daemon_ocp_valid_ma(SX127X, 135), 0);
    expect_int("SX127x 240", daemon_ocp_valid_ma(SX127X, 240), 1);
    expect_int("SX127x 250", daemon_ocp_valid_ma(SX127X, 250), 0);

    expect_int("SX1262 0 (no off)", daemon_ocp_valid_ma(SX1262, 0), 0);
    expect_int("SX1262 4", daemon_ocp_valid_ma(SX1262, 4), 0);
    expect_int("SX1262 5", daemon_ocp_valid_ma(SX1262, 5), 1);
    expect_int("SX1262 137", daemon_ocp_valid_ma(SX1262, 137), 0);
    expect_int("SX1262 62 (not /5)", daemon_ocp_valid_ma(SX1262, 62), 0);
    expect_int("SX1262 100", daemon_ocp_valid_ma(SX1262, 100), 1);
    expect_int("SX1262 140", daemon_ocp_valid_ma(SX1262, 140), 1);
    expect_int("SX1262 145", daemon_ocp_valid_ma(SX1262, 145), 0);
}

/* ---- default and effective value ------------------------------------------ */

static void test_effective(void)
{
    daemon_ocp_boot_reset();
    expect_int("default SX127x 100", daemon_ocp_default_ma(SX127X), 100);
    expect_int("default SX1262 140", daemon_ocp_default_ma(SX1262), 140);
    expect_int("not given: SX127x effective 100", daemon_ocp_effective_ma(SX127X), 100);
    expect_int("not given: SX1262 effective 140", daemon_ocp_effective_ma(SX1262), 140);

    daemon_set_ocp_boot_global("100");
    expect_int("--ocp 100: SX1262 effective 100", daemon_ocp_effective_ma(SX1262), 100);

    daemon_ocp_boot_reset();
    daemon_set_ocp_boot_global("0");
    expect_int("--ocp 0: SX127x effective 0", daemon_ocp_effective_ma(SX127X), 0);
}

/* ---- the startup notice ---------------------------------------------------- */

static const char *notice(DaemonChipFamily family, bool high_power, const char *ocp)
{
    static char buf[160];

    daemon_ocp_boot_reset();
    if (ocp)
        daemon_set_ocp_boot_global(ocp);
    if (!daemon_ocp_notice(family, high_power, buf, sizeof(buf)))
        return "(none)";
    return buf;
}

static void test_notice(void)
{
    expect_str("SX127x, no --ocp: silent", notice(SX127X, false, NULL), "(none)");
    expect_str("SX1262, no --ocp: silent", notice(SX1262, false, NULL), "(none)");
    expect_str("SX1262, no --ocp, --high-power: silent", notice(SX1262, true, NULL), "(none)");
    expect_str("SX127x, --ocp 100 (the default): silent", notice(SX127X, false, "100"), "(none)");
    expect_str("SX1262, --ocp 140 (the default): silent", notice(SX1262, false, "140"), "(none)");

    expect_str("SX127x --high-power without --ocp",
               notice(SX127X, true, NULL),
               "WARNING OCP not specified under --high-power: the 100 mA "
               "default may limit +20 dBm");
    expect_str("SX127x --ocp 0",
               notice(SX127X, false, "0"),
               "WARNING OCP disabled (--ocp 0): warranty void, hardware may be in danger");
    expect_str("SX127x --ocp 150",
               notice(SX127X, true, "150"),
               "WARNING OCP 150 mA above the default 100 mA: warranty void, "
               "hardware may be in danger");
    expect_str("SX127x --ocp 60",
               notice(SX127X, false, "60"),
               "NOTE OCP 60 mA below the default 100 mA: may limit output power");
    expect_str("SX1262 --ocp 100",
               notice(SX1262, false, "100"),
               "NOTE OCP 100 mA below the default 140 mA: may limit output power");

    char tiny[1] = { 'x' };
    daemon_ocp_boot_reset();
    daemon_set_ocp_boot_global("0");
    expect_int("zero-size buffer: no notice", daemon_ocp_notice(SX127X, false, tiny, 0), 0);
}

int main(void)
{
    test_parse();
    test_valid_sets();
    test_effective();
    test_notice();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
