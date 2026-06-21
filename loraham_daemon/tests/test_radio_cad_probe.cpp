#include "../radio_cad.h"
#include "../daemon_data_tx_runtime.h"

#include <stdio.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <thread>

/* --- Radio CAD probe helper tests --------------------------------------- */

static int g_ok = 0;
static int g_fail = 0;

struct FakeRadio {
    int scan_result;
    int scan_count;
    int callback_count;
    int start_receive_count;
    int clear_irq_count;
    float rssi;
    void (*last_callback)(void);

    FakeRadio() : scan_result(0), scan_count(0), callback_count(0),
                  start_receive_count(0), clear_irq_count(0),
                  rssi(-91.5f), last_callback(NULL) {}

    void setPacketReceivedAction(void (*cb)(void))
    {
        last_callback = cb;
        callback_count++;
    }

    void startReceive()
    {
        start_receive_count++;
    }

    void clearIrq(uint32_t)
    {
        clear_irq_count++;
    }

    int scanChannel()
    {
        scan_count++;
        return scan_result;
    }

    float getRSSI()
    {
        return rssi;
    }

    float getRSSI(bool, bool)
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
    expect_int("fsk callback not restored", ctrl.radio->callback_count, 0);
    expect_int("fsk startReceive not called", ctrl.radio->start_receive_count, 0);
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
    expect_int("lora free callback restored", ctrl.radio->callback_count, 1);
    expect_int("lora free rx restarted", ctrl.radio->start_receive_count, 1);
    expect_float_centi("lora free rssi snapshot", result.rssi_dbm, -9150);

    ctrl.radio->scan_result = 1;
    result = radio_cad_probe(&ctrl);
    expect_int("lora busy status", result.status, RADIO_CAD_PROBE_BUSY);
    expect_int("lora busy raw state", result.scan_state, 1);
    expect_int("lora busy callback restored", ctrl.radio->callback_count, 2);
    expect_int("lora busy rx restarted", ctrl.radio->start_receive_count, 2);

    ctrl.radio->scan_result = -2;
    result = radio_cad_probe(&ctrl);
    expect_int("lora error unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("lora error raw state", result.scan_state, -2);
    expect_int("lora error callback restored", ctrl.radio->callback_count, 3);
    expect_int("lora error rx restarted", ctrl.radio->start_receive_count, 3);
}



static void test_probe_preserves_broadcast_latch(void)
{
    RadioController<FakeRadio> ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.radio->scan_result = 0;

    ctrl.cad_broadcast_active.store(true);
    result = radio_cad_probe(&ctrl);
    expect_int("probe latch busy scan ran", result.scan_ran, 1);
    expect_int("probe keeps broadcast active",
               ctrl.cad_broadcast_active.load() ? 1 : 0,
               1);

    ctrl.cad_broadcast_active.store(false);
    result = radio_cad_probe(&ctrl);
    expect_int("probe latch free scan ran", result.scan_ran, 1);
    expect_int("probe keeps broadcast inactive",
               ctrl.cad_broadcast_active.load() ? 1 : 0,
               0);
}

static void test_probe_waits_for_radio_access_guard(void)
{
    RadioController<FakeRadio> ctrl;
    std::atomic<int> entered(0);
    std::atomic<int> finished(0);

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.radio->scan_result = 0;

    std::unique_lock<std::recursive_mutex> hold(ctrl.radio_mutex);
    std::thread worker([&]() {
        entered.store(1);
        RadioCadProbeResult result = radio_cad_probe(&ctrl);
        expect_int("guarded probe status", result.status, RADIO_CAD_PROBE_FREE);
        finished.store(1);
    });

    while (!entered.load())
        std::this_thread::yield();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    expect_int("guarded probe blocked scan", ctrl.radio->scan_count, 0);
    expect_int("guarded probe not finished", finished.load(), 0);

    hold.unlock();
    worker.join();

    expect_int("guarded probe scan after release", ctrl.radio->scan_count, 1);
    expect_int("guarded probe finished", finished.load(), 1);
}


static void test_try_probe_skips_active_tx(void)
{
    RadioController<FakeRadio> ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.radio->scan_result = 1;
    ctrl.tx_busy.store(true);

    result = radio_cad_try_probe(&ctrl);

    expect_int("try probe busy unavailable",
               result.status,
               RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("try probe busy scan not run", result.scan_ran, 0);
    expect_int("try probe busy no scan", ctrl.radio->scan_count, 0);
    expect_int("try probe busy cad flag clear",
               ctrl.cad_active.load() ? 1 : 0,
               0);
}

static void test_tx_wait_direct_mode_skips_cad(void)
{
    RadioController<FakeRadio> ctrl;
    DataTxDaemonContext<FakeRadio> tx;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.tx_mode = RADIO_TX_MODE_DIRECT;
    tx.ctrl = &ctrl;
    tx.log_ctx = "TEST";

    // DIRECT transmits immediately even on a busy channel: FREE, no CAD probe.
    ctrl.radio->scan_result = 1;
    expect_int("direct tx busy wait result", data_tx_wait_channel_free(&tx), 0);
    expect_int("direct tx busy no scan", ctrl.radio->scan_count, 0);

    ctrl.radio->scan_result = 0;
    expect_int("direct tx free wait result", data_tx_wait_channel_free(&tx), 0);
    expect_int("direct tx free still no scan", ctrl.radio->scan_count, 0);
}

static void test_tx_wait_fsk_skips_cad(void)
{
    RadioController<FakeRadio> ctrl;
    DataTxDaemonContext<FakeRadio> tx;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_FSK);
    tx.ctrl = &ctrl;
    tx.log_ctx = "TEST";

    ctrl.radio->scan_result = 1;
    expect_int("fsk tx wait free", data_tx_wait_channel_free(&tx), 0);
    expect_int("fsk tx wait no scan", ctrl.radio->scan_count, 0);
}

static void test_passive_probe_is_non_destructive(void)
{
    RadioController<FakeRadio> ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);

    // RSSI below threshold (-90) -> FREE, above -> BUSY, never scanning.
    ctrl.radio->rssi = -95.0f;
    result = radio_cad_probe_passive(&ctrl);
    expect_int("passive free status", result.status, RADIO_CAD_PROBE_FREE);
    expect_int("passive free scan not ran", result.scan_ran, 0);

    ctrl.radio->rssi = -80.0f;
    result = radio_cad_probe_passive(&ctrl);
    expect_int("passive busy status", result.status, RADIO_CAD_PROBE_BUSY);

    // The whole point: monitoring must never touch RX.
    expect_int("passive no scanChannel", ctrl.radio->scan_count, 0);
    expect_int("passive no startReceive", ctrl.radio->start_receive_count, 0);
    expect_int("passive no setPacketReceivedAction", ctrl.radio->callback_count, 0);
    expect_int("passive no clearIrq", ctrl.radio->clear_irq_count, 0);
}

static void test_passive_probe_non_lora_unavailable(void)
{
    RadioController<FakeRadio> ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_FSK);
    ctrl.radio->rssi = -50.0f; // would be "busy" if it mismeasured

    result = radio_cad_probe_passive(&ctrl);
    expect_int("passive fsk unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("passive fsk no scan", ctrl.radio->scan_count, 0);
    expect_int("passive fsk no startReceive", ctrl.radio->start_receive_count, 0);
}

static void test_restore_clears_received_and_irq(void)
{
    RadioController<FakeRadio> ctrl;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.received.store(true);

    radio_cad_restore_rx_after_probe(&ctrl);

    expect_int("restore clears received", ctrl.received.load() ? 1 : 0, 0);
    expect_int("restore clears irq", ctrl.radio->clear_irq_count >= 1 ? 1 : 0, 1);
    expect_int("restore re-attaches callback", ctrl.radio->callback_count, 1);
    expect_int("restore re-arms rx", ctrl.radio->start_receive_count, 1);
}

static void test_active_probe_leaves_no_spurious_received(void)
{
    RadioController<FakeRadio> ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.radio->scan_result = 0;
    ctrl.received.store(true); // simulate a stale flag set during the probe

    result = radio_cad_probe(&ctrl);

    expect_int("active probe scanned once", ctrl.radio->scan_count, 1);
    expect_int("active probe restore cleared received",
               ctrl.received.load() ? 1 : 0, 0);
    expect_int("active probe cleared irq",
               ctrl.radio->clear_irq_count >= 1 ? 1 : 0, 1);
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
    test_probe_preserves_broadcast_latch();
    test_probe_waits_for_radio_access_guard();
    test_try_probe_skips_active_tx();
    test_tx_wait_direct_mode_skips_cad();
    test_tx_wait_fsk_skips_cad();
    test_passive_probe_is_non_destructive();
    test_passive_probe_non_lora_unavailable();
    test_restore_clears_received_and_irq();
    test_active_probe_leaves_no_spurious_received();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
