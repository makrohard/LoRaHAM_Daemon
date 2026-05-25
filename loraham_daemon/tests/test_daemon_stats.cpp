#include "../daemon_stats.h"

#include <stdio.h>
#include <string.h>

/* --- Runtime statistics tests --- */

static int g_ok = 0;
static int g_fail = 0;

static void expect_ulong(const char *name, unsigned long actual, unsigned long expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %lu, got %lu\n", name, expected, actual);
    }
}

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

static void test_tx_result_counters(void)
{
    DaemonRadioStats stats;

    daemon_radio_stats_init(&stats);

    daemon_radio_stats_record_tx_result(&stats, TX_RESULT_OK);
    daemon_radio_stats_record_tx_result(&stats, TX_RESULT_BUSY);
    daemon_radio_stats_record_tx_result(&stats, TX_RESULT_RADIO_ERROR);

    expect_ulong("tx ok once", stats.tx_ok, 1);
    expect_ulong("tx busy once", stats.tx_busy, 1);
    expect_ulong("tx error once", stats.tx_errors, 1);
    expect_ulong("cad untouched", stats.cad_timeouts, 0);
}

static void test_cad_timeout_counted_once_via_tx_result(void)
{
    DaemonRadioStats stats;

    daemon_radio_stats_init(&stats);

    daemon_radio_stats_record_tx_result(&stats, TX_RESULT_CAD_TIMEOUT);

    expect_ulong("cad timeout once", stats.cad_timeouts, 1);
    expect_ulong("cad timeout not tx error", stats.tx_errors, 0);
    expect_ulong("cad timeout not busy", stats.tx_busy, 0);
}

static void test_stats_format_includes_cad_timeout(void)
{
    DaemonRadioStats stats;
    char buf[256];

    daemon_radio_stats_init(&stats);
    daemon_radio_stats_record_tx_result(&stats, TX_RESULT_CAD_TIMEOUT);

    daemon_stats_format_response(buf, sizeof(buf), 12, RADIO_HEALTH_READY, &stats);

    expect_int("stats has cad timeout", strstr(buf, "CADTIMEOUT=1") != NULL, 1);
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

    test_tx_result_counters();
    test_cad_timeout_counted_once_via_tx_result();
    test_stats_format_includes_cad_timeout();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
