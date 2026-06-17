#include "../radio_cad.h"

#include <stdio.h>
#include <string.h>

/* --- Radio CAD probe helper tests --------------------------------------- */

static int g_ok = 0;
static int g_fail = 0;

struct FakeRadio {
    int scan_result;
    int scan_count;
    float rssi;

    FakeRadio() : scan_result(0), scan_count(0), rssi(-91.5f) {}

    int scanChannel()
    {
        scan_count++;
        return scan_result;
    }

    float getRSSI()
    {
        return rssi;
    }
};

static void fake_rx_callback(void)
{
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

static void expect_str(const char *name, const char *actual, const char *expected)
{
    if (strcmp(actual, expected) == 0) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected '%s', got '%s'\n", name, expected, actual);
    }
}

static void expect_float_centi(const char *name, float actual, int expected_centi)
{
    int actual_centi = (int)(actual * 100.0f + (actual >= 0 ? 0.5f : -0.5f));

    expect_int(name, actual_centi, expected_centi);
}

static void init_ctrl(RadioController<FakeRadio> *ctrl,
                      RadioHealth health,
                      RadioMode_t mode)
{
    radio_controller_init(ctrl,
                          RADIO_BAND_433,
                          "TEST",
                          false,
                          fake_rx_callback,
                          13);
    ctrl->radio.reset(new FakeRadio());
    ctrl->health = health;
    ctrl->mode = mode;
}

static void test_status_names(void)
{
    expect_str("cad status unavailable name",
               radio_cad_probe_status_name(RADIO_CAD_PROBE_UNAVAILABLE),
               "UNAVAILABLE");
    expect_str("cad status free name",
               radio_cad_probe_status_name(RADIO_CAD_PROBE_FREE),
               "FREE");
    expect_str("cad status busy name",
               radio_cad_probe_status_name(RADIO_CAD_PROBE_BUSY),
               "BUSY");
}

static void test_scan_state_mapping(void)
{
    expect_int("scan state zero free",
               radio_cad_status_from_scan_state(0),
               RADIO_CAD_PROBE_FREE);
    expect_int("scan state positive busy",
               radio_cad_status_from_scan_state(1),
               RADIO_CAD_PROBE_BUSY);
    expect_int("scan state negative unavailable",
               radio_cad_status_from_scan_state(-2),
               RADIO_CAD_PROBE_UNAVAILABLE);
}

static void test_probe_null_and_not_ready(void)
{
    RadioController<FakeRadio> ctrl;
    RadioCadProbeResult result;

    result = radio_cad_probe<FakeRadio>(NULL);
    expect_int("null probe unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("null probe scan not run", result.scan_ran, 0);

    init_ctrl(&ctrl, RADIO_HEALTH_FAILED, RADIO_MODE_LORA);
    ctrl.radio->scan_result = 0;

    result = radio_cad_probe(&ctrl);
    expect_int("failed radio probe unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("failed radio scan count", ctrl.radio->scan_count, 0);
}

static void test_probe_fsk_has_rssi_no_cad(void)
{
    RadioController<FakeRadio> ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_FSK);
    ctrl.radio->rssi = -88.25f;

    result = radio_cad_probe(&ctrl);
    expect_int("fsk cad unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("fsk scan not run", result.scan_ran, 0);
    expect_int("fsk scan count", ctrl.radio->scan_count, 0);
    expect_float_centi("fsk rssi snapshot", result.rssi_dbm, -8825);
}

static void test_probe_lora_free_busy_error(void)
{
    RadioController<FakeRadio> ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.radio->scan_result = 0;
    ctrl.radio->rssi = -91.5f;

    result = radio_cad_probe(&ctrl);
    expect_int("lora free status", result.status, RADIO_CAD_PROBE_FREE);
    expect_int("lora free scan ran", result.scan_ran, 1);
    expect_int("lora free scan count", ctrl.radio->scan_count, 1);
    expect_int("lora free cad flag cleared", ctrl.cad_active.load() ? 1 : 0, 0);
    expect_int("lora free raw state", result.scan_state, 0);
    expect_float_centi("lora free rssi snapshot", result.rssi_dbm, -9150);

    ctrl.radio->scan_result = 1;
    result = radio_cad_probe(&ctrl);
    expect_int("lora busy status", result.status, RADIO_CAD_PROBE_BUSY);
    expect_int("lora busy raw state", result.scan_state, 1);

    ctrl.radio->scan_result = -2;
    result = radio_cad_probe(&ctrl);
    expect_int("lora error unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("lora error raw state", result.scan_state, -2);
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

    test_status_names();
    test_scan_state_mapping();
    test_probe_null_and_not_ready();
    test_probe_fsk_has_rssi_no_cad();
    test_probe_lora_free_busy_error();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
