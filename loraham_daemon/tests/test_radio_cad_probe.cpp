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

// Fake-Treiber: überschreibt die virtuellen RadioDriver-Delegates und zählt
// Aufrufe wie der frühere Template-Fake (Zähler-Semantik unverändert).
struct FakeRadio : public RadioDriver {
    int scan_result;
    int scan_count;
    int callback_count;
    int start_receive_count;
    int clear_irq_count;
    float rssi;
    void (*last_callback)(void);

    FakeRadio() : RadioDriver(NULL),
                  scan_result(0), scan_count(0), callback_count(0),
                  start_receive_count(0), clear_irq_count(0),
                  rssi(-91.5f), last_callback(NULL) {}

    void setPacketReceivedAction(void (*cb)(void)) override
    {
        last_callback = cb;
        callback_count++;
    }

    int16_t startReceive() override
    {
        start_receive_count++;
        return 0;
    }

    int16_t clearIrq(uint32_t) override
    {
        clear_irq_count++;
        return 0;
    }

    int16_t scanChannel() override
    {
        scan_count++;
        return scan_result;
    }

    float getRSSI() override
    {
        return rssi;
    }

    float rssiProbe() override
    {
        return rssi;
    }

    int16_t begin(const RadioRfDefaults *) override { return 0; }
    int16_t switchMode(RadioMode_t) override { return 0; }
    void applyLoraParam(const char *, const std::string &,
                        const std::string &) override {}
    void applyFskParam(const char *, const std::string &,
                       const std::string &) override {}
    float readLiveRssi(RadioMode_t, bool) override { return -200.0f; }
    const char *chipName() const override { return "FAKE"; }
};

static FakeRadio *fake(RadioController *ctrl)
{
    return static_cast<FakeRadio *>(ctrl->driver.get());
}

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

static void init_ctrl(RadioController *ctrl,
                      RadioHealth health,
                      RadioMode_t mode)
{
    radio_controller_init(ctrl,
                          RADIO_BAND_433,
                          "TEST",
                          false,
                          fake_rx_callback,
                          13);
    ctrl->driver.reset(new FakeRadio());
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
    RadioController ctrl;
    RadioCadProbeResult result;

    result = radio_cad_probe(NULL);
    expect_int("null probe unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("null probe scan not run", result.scan_ran, 0);

    init_ctrl(&ctrl, RADIO_HEALTH_FAILED, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;

    result = radio_cad_probe(&ctrl);
    expect_int("failed radio probe unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("failed radio scan count", fake(&ctrl)->scan_count, 0);
}

static void test_probe_fsk_has_rssi_no_cad(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_FSK);
    fake(&ctrl)->rssi = -88.25f;

    result = radio_cad_probe(&ctrl);
    expect_int("fsk cad unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("fsk scan not run", result.scan_ran, 0);
    expect_int("fsk scan count", fake(&ctrl)->scan_count, 0);
    expect_int("fsk callback not restored", fake(&ctrl)->callback_count, 0);
    expect_int("fsk startReceive not called", fake(&ctrl)->start_receive_count, 0);
    expect_float_centi("fsk rssi snapshot", result.rssi_dbm, -8825);
}

static void test_probe_lora_free_busy_error(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;
    fake(&ctrl)->rssi = -91.5f;

    result = radio_cad_probe(&ctrl);
    expect_int("lora free status", result.status, RADIO_CAD_PROBE_FREE);
    expect_int("lora free scan ran", result.scan_ran, 1);
    expect_int("lora free scan count", fake(&ctrl)->scan_count, 1);
    expect_int("lora free cad flag cleared", ctrl.cad_active.load() ? 1 : 0, 0);
    expect_int("lora free raw state", result.scan_state, 0);
    expect_int("lora free callback restored", fake(&ctrl)->callback_count, 1);
    expect_int("lora free rx restarted", fake(&ctrl)->start_receive_count, 1);
    expect_float_centi("lora free rssi snapshot", result.rssi_dbm, -9150);

    fake(&ctrl)->scan_result = 1;
    result = radio_cad_probe(&ctrl);
    expect_int("lora busy status", result.status, RADIO_CAD_PROBE_BUSY);
    expect_int("lora busy raw state", result.scan_state, 1);
    expect_int("lora busy callback restored", fake(&ctrl)->callback_count, 2);
    expect_int("lora busy rx restarted", fake(&ctrl)->start_receive_count, 2);

    fake(&ctrl)->scan_result = -2;
    result = radio_cad_probe(&ctrl);
    expect_int("lora error unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("lora error raw state", result.scan_state, -2);
    expect_int("lora error callback restored", fake(&ctrl)->callback_count, 3);
    expect_int("lora error rx restarted", fake(&ctrl)->start_receive_count, 3);
}



static void test_probe_preserves_broadcast_latch(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;

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
    RadioController ctrl;
    std::atomic<int> entered(0);
    std::atomic<int> finished(0);

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;

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
    expect_int("guarded probe blocked scan", fake(&ctrl)->scan_count, 0);
    expect_int("guarded probe not finished", finished.load(), 0);

    hold.unlock();
    worker.join();

    expect_int("guarded probe scan after release", fake(&ctrl)->scan_count, 1);
    expect_int("guarded probe finished", finished.load(), 1);
}


static void test_try_probe_skips_active_tx(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 1;
    ctrl.tx_busy.store(true);

    result = radio_cad_try_probe(&ctrl);

    expect_int("try probe busy unavailable",
               result.status,
               RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("try probe busy scan not run", result.scan_ran, 0);
    expect_int("try probe busy no scan", fake(&ctrl)->scan_count, 0);
    expect_int("try probe busy cad flag clear",
               ctrl.cad_active.load() ? 1 : 0,
               0);
}

/* Wiring without DIO1 (cad_scan_available=false): probes must never call
 * scanChannel and must answer from the passive RSSI probe instead. */
static void test_probe_gated_without_dio1(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.cad_scan_available = false;
    ctrl.cad_rssi_threshold_dbm.store(-90.0f);

    fake(&ctrl)->rssi = -70.0f;              /* above threshold: busy */
    result = radio_cad_probe(&ctrl);
    expect_int("gated probe busy", result.status, RADIO_CAD_PROBE_BUSY);
    expect_int("gated probe no scan flag", result.scan_ran, 0);
    expect_int("gated probe no scanChannel", fake(&ctrl)->scan_count, 0);
    expect_int("gated probe no rx re-arm", fake(&ctrl)->start_receive_count, 0);

    fake(&ctrl)->rssi = -110.0f;             /* below threshold: free */
    result = radio_cad_probe(&ctrl);
    expect_int("gated probe free", result.status, RADIO_CAD_PROBE_FREE);
    expect_int("gated probe still no scanChannel", fake(&ctrl)->scan_count, 0);

    result = radio_cad_try_probe(&ctrl);
    expect_int("gated try probe free", result.status, RADIO_CAD_PROBE_FREE);
    expect_int("gated try probe no scan flag", result.scan_ran, 0);
    expect_int("gated try probe no scanChannel", fake(&ctrl)->scan_count, 0);
}

static void test_tx_wait_direct_mode_skips_cad(void)
{
    RadioController ctrl;
    DataTxDaemonContext tx;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.tx_mode = RADIO_TX_MODE_DIRECT;
    tx.ctrl = &ctrl;
    tx.log_ctx = "TEST";

    // DIRECT transmits immediately even on a busy channel: FREE, no CAD probe.
    fake(&ctrl)->scan_result = 1;
    expect_int("direct tx busy wait result", data_tx_wait_channel_free(&tx), 0);
    expect_int("direct tx busy no scan", fake(&ctrl)->scan_count, 0);

    fake(&ctrl)->scan_result = 0;
    expect_int("direct tx free wait result", data_tx_wait_channel_free(&tx), 0);
    expect_int("direct tx free still no scan", fake(&ctrl)->scan_count, 0);
}

static void test_tx_wait_fsk_skips_cad(void)
{
    RadioController ctrl;
    DataTxDaemonContext tx;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_FSK);
    tx.ctrl = &ctrl;
    tx.log_ctx = "TEST";

    fake(&ctrl)->scan_result = 1;
    expect_int("fsk tx wait free", data_tx_wait_channel_free(&tx), 0);
    expect_int("fsk tx wait no scan", fake(&ctrl)->scan_count, 0);
}

static void test_passive_probe_is_non_destructive(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);

    // RSSI below threshold (-90) -> FREE, above -> BUSY, never scanning.
    fake(&ctrl)->rssi = -95.0f;
    result = radio_cad_probe_passive(&ctrl);
    expect_int("passive free status", result.status, RADIO_CAD_PROBE_FREE);
    expect_int("passive free scan not ran", result.scan_ran, 0);

    fake(&ctrl)->rssi = -80.0f;
    result = radio_cad_probe_passive(&ctrl);
    expect_int("passive busy status", result.status, RADIO_CAD_PROBE_BUSY);

    // The whole point: monitoring must never touch RX.
    expect_int("passive no scanChannel", fake(&ctrl)->scan_count, 0);
    expect_int("passive no startReceive", fake(&ctrl)->start_receive_count, 0);
    expect_int("passive no setPacketReceivedAction", fake(&ctrl)->callback_count, 0);
    expect_int("passive no clearIrq", fake(&ctrl)->clear_irq_count, 0);
}

static void test_passive_probe_uses_per_band_threshold(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);

    // Lower the threshold: -98 dBm is now above -100 -> BUSY.
    ctrl.cad_rssi_threshold_dbm.store(-100.0f);
    fake(&ctrl)->rssi = -98.0f;
    result = radio_cad_probe_passive(&ctrl);
    expect_int("low threshold busy", result.status, RADIO_CAD_PROBE_BUSY);

    // Raise the threshold: -98 dBm is now below -70 -> FREE.
    ctrl.cad_rssi_threshold_dbm.store(-70.0f);
    result = radio_cad_probe_passive(&ctrl);
    expect_int("high threshold free", result.status, RADIO_CAD_PROBE_FREE);

    expect_int("threshold test no scan", fake(&ctrl)->scan_count, 0);
}

static void test_passive_probe_non_lora_unavailable(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_FSK);
    fake(&ctrl)->rssi = -50.0f; // would be "busy" if it mismeasured

    result = radio_cad_probe_passive(&ctrl);
    expect_int("passive fsk unavailable", result.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("passive fsk no scan", fake(&ctrl)->scan_count, 0);
    expect_int("passive fsk no startReceive", fake(&ctrl)->start_receive_count, 0);
}

static void test_restore_clears_received_and_irq(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.received.store(true);

    radio_cad_restore_rx_after_probe(&ctrl);

    expect_int("restore clears received", ctrl.received.load() ? 1 : 0, 0);
    expect_int("restore clears irq", fake(&ctrl)->clear_irq_count >= 1 ? 1 : 0, 1);
    expect_int("restore re-attaches callback", fake(&ctrl)->callback_count, 1);
    expect_int("restore re-arms rx", fake(&ctrl)->start_receive_count, 1);
}

static void test_active_probe_leaves_no_spurious_received(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;
    ctrl.received.store(true); // simulate a stale flag set during the probe

    result = radio_cad_probe(&ctrl);

    expect_int("active probe scanned once", fake(&ctrl)->scan_count, 1);
    expect_int("active probe restore cleared received",
               ctrl.received.load() ? 1 : 0, 0);
    expect_int("active probe cleared irq",
               fake(&ctrl)->clear_irq_count >= 1 ? 1 : 0, 1);
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
    test_probe_gated_without_dio1();
    test_tx_wait_direct_mode_skips_cad();
    test_tx_wait_fsk_skips_cad();
    test_passive_probe_is_non_destructive();
    test_passive_probe_uses_per_band_threshold();
    test_passive_probe_non_lora_unavailable();
    test_restore_clears_received_and_irq();
    test_active_probe_leaves_no_spurious_received();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
