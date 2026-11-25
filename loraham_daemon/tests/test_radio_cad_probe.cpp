#include "../radio_cad.h"
#include "../daemon_band.h"
#include "../daemon_data_tx_runtime.h"
#include "../daemon_rx_rearm.h"
#include "../daemon_tx_executor.h"

#include <stdio.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <thread>

/* --- Radio CAD probe helper tests --------------------------------------- */

static int g_ok = 0;
static int g_fail = 0;

// Fake driver: overrides the virtual RadioDriver delegates and counts calls
// exactly as the earlier template fake did (counter semantics unchanged).
struct FakeRadio : public RadioDriver {
    int scan_result;
    int scan_count;
    int callback_count;
    int start_receive_count;
    int clear_irq_count;
    int clear_callback_count;
    bool callback_attached;
    bool attached_during_scan;
    int rssi_probe_count;
    int get_rssi_count;
    float rssi;
    void (*last_callback)(void);

    FakeRadio() : RadioDriver(NULL),
                  scan_result(0), scan_count(0), callback_count(0),
                  start_receive_count(0), clear_irq_count(0),
                  clear_callback_count(0), callback_attached(false),
                  attached_during_scan(false),
                  rssi_probe_count(0), get_rssi_count(0),
                  rssi(-91.5f), last_callback(NULL) {}

    void setPacketReceivedAction(void (*cb)(void)) override
    {
        last_callback = cb;
        callback_count++;
        callback_attached = true;
    }

    void clearPacketReceivedAction() override
    {
        clear_callback_count++;
        callback_attached = false;
        /* Recorded at the moment the scan runs, which is what the ordering
         * assertion below is actually about. */
    }

    int16_t startReceive() override
    {
        start_receive_count++;
        return 0;
    }

    int16_t clearIrq(uint32_t) override
    {
        clear_irq_count++;
        if (rx_progress_latched)
            rx_progress = RADIO_RX_PROGRESS_NONE;
        return 0;
    }

    int16_t scanChannel() override
    {
        scan_count++;
        if (callback_attached)
            attached_during_scan = true;
        return scan_result;
    }

    /*
     * Scripted chip-level RxDone. Two knobs, because the window moved:
     *
     *   rx_done_after_checks  the chip reports a packet from the Nth query on
     *   rx_done_on_standby    a packet completes DURING the quiesce -- the
     *                         case that matters now, because the probe stops
     *                         reception before it asks
     */
    int rx_done_queries = 0;
    int rx_done_after_checks = -1;   /* -1 = never */
    bool rx_done_on_standby = false;
    bool rx_done_latched = false;

    int standby_count = 0;
    int16_t standby_result = 0;

    int16_t standby() override
    {
        standby_count++;
        if (rx_done_on_standby)
            rx_done_latched = true;   /* the packet landed as we stopped RX */
        return standby_result;
    }

    bool rxDonePending() override
    {
        const bool scripted = (rx_done_after_checks >= 0 &&
                               rx_done_queries >= rx_done_after_checks);
        rx_done_queries++;
        return scripted || rx_done_latched;
    }

    /*
     * Scripted frame in progress. `rx_progress_latched` = the SX126x kind,
     * which the probe restore's clearIrq() wipes; false = the SX127x kind,
     * a live status that stays as scripted. Airtime: len x 100 ms, so the
     * stage bounds are 400 ms (4 B) and 25.5 s (255 B), far from the
     * microseconds a probe takes.
     */
    RadioRxProgress rx_progress = RADIO_RX_PROGRESS_NONE;
    bool rx_progress_latched = true;
    size_t last_airtime_len = 0;

    RadioRxProgress rxProgress() override { return rx_progress; }

    RadioLibTime_t getTimeOnAir(size_t len) override
    {
        last_airtime_len = len;
        return (RadioLibTime_t)len * 100000UL;
    }

    float getRSSI() override
    {
        get_rssi_count++;
        return rssi;
    }

    float rssiProbe() override
    {
        rssi_probe_count++;
        return rssi;
    }

    int16_t begin(const RadioRfDefaults *) override { return 0; }
    int16_t switchMode(RadioMode_t,
                       const RadioRfDefaults *) override { return 0; }
    int16_t applyLoraParam(const char *, const std::string &,
                           const std::string &) override { return 0; }
    int16_t applyFskParam(const char *, const std::string &,
                          const std::string &) override { return 0; }
    float readLiveRssi(RadioMode_t, bool) override { return -200.0f; }
    const char *chipName() const override { return "FAKE"; }
    DaemonChipFamily chipFamily() const override
    {
        return DAEMON_CHIP_FAMILY_SX127X;
    }
};

static FakeRadio *fake(RadioController *ctrl)
{
    return static_cast<FakeRadio *>(ctrl->driver.get());
}

/* Link stub: daemon_data_tx_runtime.cpp references the production sender. */
TxResult lora_send(uint8_t *buf, size_t len, int band)
{
    (void)buf;
    (void)len;
    (void)band;
    return TX_RESULT_RADIO_ERROR;
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

    /* HW-3: not merely "no verdict" but no radio call AT ALL. The reading is
     * the passive probe's entire output, so taking it and then discarding it
     * would leave a value that describes the wrong modem one edit away from
     * being used. */
    expect_int("passive fsk makes zero radio probe calls",
               fake(&ctrl)->rssi_probe_count + fake(&ctrl)->get_rssi_count, 0);
}

/*
 * HW-3, the second condition. The datasheet defines RSSI only while the
 * receiver has been active for TS_RE + TS_RSSI, so a value read in standby is
 * undefined. rx_rearm_pending is exactly that window, and it is a real state,
 * not a hypothetical: the daemon retries a failed re-arm before escalating to
 * FAILED, and config_status.cpp folds the flag into RXREADY, so READY with the
 * receiver not armed is reachable. Without the guard an undefined reading
 * becomes a FREE verdict -- and a false FREE is a transmission on top of
 * whoever is already on the channel.
 */
static void test_passive_probe_during_rx_rearm_unavailable(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->rssi = -120.0f;   /* would read as a quiet channel */
    ctrl.rx_rearm_pending.store(true);

    result = radio_cad_probe_passive(&ctrl);
    expect_int("passive probe during re-arm is unavailable", result.status,
               RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("passive probe during re-arm makes zero radio probe calls",
               fake(&ctrl)->rssi_probe_count + fake(&ctrl)->get_rssi_count, 0);

    /* And the gate lifts again once the receiver is back. */
    ctrl.rx_rearm_pending.store(false);
    result = radio_cad_probe_passive(&ctrl);
    expect_int("passive probe works again after the re-arm", result.status,
               RADIO_CAD_PROBE_FREE);
}

/*
 * The active probes keep their non-destructive RSSI snapshot -- GET CHANNEL
 * reports it in every mode -- but must not run the CAD or the RX re-arm that
 * follows it while the receiver is not armed.
 */
static void test_active_probe_during_rx_rearm_does_not_scan(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.cad_scan_available = true;
    fake(&ctrl)->scan_result = 0;   /* would be FREE if it ran */
    ctrl.rx_rearm_pending.store(true);

    result = radio_cad_probe(&ctrl);
    expect_int("active probe during re-arm is unavailable", result.status,
               RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("active probe during re-arm did not scan",
               fake(&ctrl)->scan_count, 0);
    expect_int("active probe during re-arm did not re-arm RX",
               fake(&ctrl)->start_receive_count, 0);
    expect_int("active probe during re-arm reports no scan",
               result.scan_ran, 0);

    result = radio_cad_try_probe(&ctrl);
    expect_int("try_probe during re-arm is unavailable", result.status,
               RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("try_probe during re-arm did not scan",
               fake(&ctrl)->scan_count, 0);
}

/*
 * The regression the live matrix caught, and the software suite did not.
 *
 * RadioLib's startChannelScan() remaps DIO0 from RxDone to CadDone. The
 * daemon's packet-received alert sits on that same pin, so CAD completion fired
 * the RX callback, `received` was set, and the main loop read a FIFO still
 * holding the PREVIOUS packet -- delivering it to every client a second time.
 *
 * Measured between the two boxes: ten unique frames arrived as sixteen
 * receptions, five exact duplicates with identical RSSI and SNR. The baseline
 * daemon on the same board duplicated none, because its blocking scanChannel()
 * spins on sched_yield() and never lets the alert thread run; polling the
 * register with a real 1 ms sleep does, which turned a latent race into a
 * reliable defect.
 *
 * So the probe must take the alert OFF DIO0 before the scan, exactly as the TX
 * path has always done, and put it back after.
 */
static void test_probe_detaches_rx_callback_around_the_scan(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.cad_scan_available = true;
    fake(&ctrl)->scan_result = 0;

    /* The daemon arms the callback at boot; model that starting state. */
    ctrl.driver->setPacketReceivedAction(fake_rx_callback);
    fake(&ctrl)->attached_during_scan = false;

    (void)radio_cad_probe(&ctrl);

    expect_int("the scan ran", fake(&ctrl)->scan_count, 1);
    expect_int("the RX alert was taken off DIO0 before the scan",
               fake(&ctrl)->attached_during_scan ? 1 : 0, 0);
    expect_int("the alert was cleared exactly once",
               fake(&ctrl)->clear_callback_count, 1);
    expect_int("and reinstalled afterwards",
               fake(&ctrl)->callback_attached ? 1 : 0, 1);

    /* Same for the non-blocking entry point. */
    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.cad_scan_available = true;
    fake(&ctrl)->scan_result = 0;
    ctrl.driver->setPacketReceivedAction(fake_rx_callback);
    fake(&ctrl)->attached_during_scan = false;

    (void)radio_cad_try_probe(&ctrl);

    expect_int("try_probe: the scan ran", fake(&ctrl)->scan_count, 1);
    expect_int("try_probe: the RX alert was off DIO0 during the scan",
               fake(&ctrl)->attached_during_scan ? 1 : 0, 0);
    expect_int("try_probe: and reinstalled afterwards",
               fake(&ctrl)->callback_attached ? 1 : 0, 1);
}

/*
 * P1-A. The audit found my first HW-3 implementation was wrong in the active
 * paths: it kept the RSSI snapshot ahead of the gate on the reasoning that a
 * snapshot is a harmless register read. In FSK it is not.
 * RadioDriver::getRSSI() delegates to RadioLib's ordinary getRSSI(), whose FSK
 * branch calls startReceive() -- rewriting the DIO mapping and clearing ALL
 * IRQ flags, which can discard a received packet -- then standby(). Only
 * rssiProbe() (getRSSI(false, true)) skips the receive.
 *
 * GET CHANNEL runs through radio_cad_try_probe, so this was live.
 */
static void test_active_probe_in_fsk_uses_only_the_nondestructive_read(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_FSK);
    ctrl.cad_scan_available = true;
    fake(&ctrl)->rssi = -84.5f;

    result = radio_cad_try_probe(&ctrl);

    expect_int("fsk active probe is unavailable", result.status,
               RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("fsk active probe still reports an RSSI",
               (int)(result.rssi_dbm * 100.0f), -8450);
    expect_int("it used the skip-receive probe", fake(&ctrl)->rssi_probe_count, 1);
    expect_int("and NEVER the ordinary getRSSI, which re-enters RX in FSK",
               fake(&ctrl)->get_rssi_count, 0);
    expect_int("no scan", fake(&ctrl)->scan_count, 0);
    expect_int("no re-arm", fake(&ctrl)->start_receive_count, 0);

    /* Same for the blocking entry point, which the MANAGED TX gate uses. */
    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_FSK);
    ctrl.cad_scan_available = true;
    result = radio_cad_probe(&ctrl);
    expect_int("blocking probe: skip-receive only",
               fake(&ctrl)->rssi_probe_count, 1);
    expect_int("blocking probe: no ordinary getRSSI",
               fake(&ctrl)->get_rssi_count, 0);
    expect_int("blocking probe: no re-arm",
               fake(&ctrl)->start_receive_count, 0);
}

/* A pending re-arm must stop the active probe before ANY radio call, including
 * the snapshot -- the reading is undefined while the receiver is not armed. */
static void test_active_probe_during_rearm_makes_no_radio_call_at_all(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.cad_scan_available = true;
    ctrl.rx_rearm_pending.store(true);

    RadioCadProbeResult result = radio_cad_try_probe(&ctrl);

    expect_int("re-arm pending -> unavailable", result.status,
               RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("and zero radio calls of any kind",
               fake(&ctrl)->rssi_probe_count + fake(&ctrl)->get_rssi_count +
                   fake(&ctrl)->scan_count + fake(&ctrl)->start_receive_count, 0);
}

/*
 * P1-B, after the audit narrowed it further. Asking the chip for RxDone is only
 * meaningful once the receiver can no longer produce a new one. The first
 * version asked while RX was still running, which moved the window rather than
 * closing it: the query could return false and a packet complete microseconds
 * later, before the probe detached DIO0 and entered CAD.
 *
 * So the probe now calls standby() first and asks afterwards. This test models
 * exactly the surviving case: nothing pending when the probe starts, and a
 * packet completing DURING the quiesce.
 */
static void test_a_packet_completing_during_the_quiesce_is_preserved(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.cad_scan_available = true;
    ctrl.received.store(false);              /* nothing pending at entry */
    fake(&ctrl)->rx_done_after_checks = -1;  /* and the chip says so too... */
    fake(&ctrl)->rx_done_on_standby = true;  /* ...until the receiver stops */
    fake(&ctrl)->scan_result = 0;

    RadioCadProbeResult result = radio_cad_try_probe(&ctrl);

    expect_int("the receiver was stopped before the decision",
               fake(&ctrl)->standby_count >= 1, 1);
    expect_int("a packet that landed during the quiesce is reported BUSY",
               result.status, RADIO_CAD_PROBE_BUSY);
    expect_int("the software flag is set so the main loop will drain it",
               ctrl.received.load() ? 1 : 0, 1);
    expect_int("no scan ran", fake(&ctrl)->scan_count, 0);
    expect_int("the IRQ flags were not cleared",
               fake(&ctrl)->clear_irq_count, 0);
    expect_int("the RX callback was never detached",
               fake(&ctrl)->clear_callback_count, 0);
    expect_int("and the receiver was not re-armed over the packet",
               fake(&ctrl)->start_receive_count, 0);

    /* The blocking entry point, used by the MANAGED TX gate, must behave the
     * same -- it shares the helper but has its own call site. */
    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.cad_scan_available = true;
    fake(&ctrl)->rx_done_on_standby = true;
    fake(&ctrl)->scan_result = 0;
    result = radio_cad_probe(&ctrl);
    expect_int("blocking probe: BUSY", result.status, RADIO_CAD_PROBE_BUSY);
    expect_int("blocking probe: no scan", fake(&ctrl)->scan_count, 0);
    expect_int("blocking probe: no re-arm", fake(&ctrl)->start_receive_count, 0);
    expect_int("blocking probe: no IRQ clear", fake(&ctrl)->clear_irq_count, 0);

    /* Positive control: nothing arrives at any point -> the scan proceeds. */
    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.cad_scan_available = true;
    fake(&ctrl)->scan_result = 0;
    result = radio_cad_try_probe(&ctrl);
    expect_int("with nothing pending the scan runs", fake(&ctrl)->scan_count, 1);
    expect_int("and answers FREE", result.status, RADIO_CAD_PROBE_FREE);
}

/*
 * Fail closed on a radio that will not stop: no scan on a chip whose state we
 * could not establish, and no verdict invented for it.
 */
static void test_a_radio_that_will_not_quiesce_is_unavailable(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.cad_scan_available = true;
    fake(&ctrl)->standby_result = -1;

    RadioCadProbeResult result = radio_cad_try_probe(&ctrl);

    expect_int("a radio that will not stop is UNAVAILABLE", result.status,
               RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("no scan on it", fake(&ctrl)->scan_count, 0);
    expect_int("and nothing was re-armed", fake(&ctrl)->start_receive_count, 0);
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

// Ported for the pending-RX guard (audit M1): a PRE-SET received flag now
// means "undrained packet" and blocks the scan (covered by the guard tests
// below). What remains guaranteed here: without pending RX the probe scans,
// and the restore leaves no spurious received flag behind (a flag raised
// DURING the scan is cleared by radio_cad_restore_rx_after_probe, covered by
// test_restore_clears_received_and_irq).
static void test_active_probe_leaves_no_spurious_received(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;

    result = radio_cad_probe(&ctrl);

    expect_int("active probe scanned once", fake(&ctrl)->scan_count, 1);
    expect_int("active probe leaves received clear",
               ctrl.received.load() ? 1 : 0, 0);
    expect_int("active probe cleared irq",
               fake(&ctrl)->clear_irq_count >= 1 ? 1 : 0, 1);
    expect_int("active probe scan ran", result.scan_ran, 1);
}


/* --- Pending-RX guard (audit M1) ----------------------------------------- */
// A fully received, undrained packet must never be destroyed by a scan probe:
// the probe reports BUSY without scanning or touching IRQ/flag/RX state.

static void test_probe_pending_rx_returns_busy(void)
{
    RadioController ctrl;
    RadioCadProbeResult result;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->rssi = -77.0f;
    ctrl.received.store(true);

    result = radio_cad_probe(&ctrl);
    expect_int("pending rx probe busy", result.status, RADIO_CAD_PROBE_BUSY);
    expect_int("pending rx probe no scan", result.scan_ran, 0);
    expect_int("pending rx probe scan count", fake(&ctrl)->scan_count, 0);
    expect_int("pending rx probe no clearIrq", fake(&ctrl)->clear_irq_count, 0);
    expect_int("pending rx probe no startReceive",
               fake(&ctrl)->start_receive_count, 0);
    expect_int("pending rx probe flag intact",
               ctrl.received.load() ? 1 : 0, 1);
    expect_float_centi("pending rx probe passive rssi", result.rssi_dbm, -7700);

    result = radio_cad_try_probe(&ctrl);
    expect_int("pending rx try probe busy", result.status, RADIO_CAD_PROBE_BUSY);
    expect_int("pending rx try probe no scan", result.scan_ran, 0);
    expect_int("pending rx try probe scan count", fake(&ctrl)->scan_count, 0);
    expect_int("pending rx try probe flag intact",
               ctrl.received.load() ? 1 : 0, 1);
}

static void test_managed_wait_pending_rx_blocks(void)
{
    RadioController ctrl;
    DataTxDaemonContext tx;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.tx_mode = RADIO_TX_MODE_MANAGED;
    tx.ctrl = &ctrl;
    tx.log_ctx = "TEST";

    // Packet arrives (and stays undrained) before/through the CAD wait.
    ctrl.received.store(true);
    fake(&ctrl)->scan_result = 0;   // channel would read FREE if scanned

    expect_int("managed wait pending rx blocks",
               data_tx_wait_channel_free_with_limits_ex(&tx, 3, 1, 0, false),
               DATA_TX_CAD_WAIT_BLOCK);
    expect_int("managed wait pending rx never scanned",
               fake(&ctrl)->scan_count, 0);
    expect_int("managed wait pending rx no clearIrq",
               fake(&ctrl)->clear_irq_count, 0);
    expect_int("managed wait pending rx packet survives",
               ctrl.received.load() ? 1 : 0, 1);
}

/* --- A frame still arriving (radio_cad_rx_in_progress) ------------------- */

/* Nothing touched: no standby, no IRQ clear, no detach, no scan, no re-arm,
 * and `received` stays as it was (the frame has not ended). */
static void expect_receiver_untouched(const char *what, RadioController *ctrl,
                                      const RadioCadProbeResult &r)
{
    char name[160];

#define UNTOUCHED(label, got, want) \
    snprintf(name, sizeof(name), "%s: %s", what, label); \
    expect_int(name, got, want)
    UNTOUCHED("BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    UNTOUCHED("no scan", r.scan_ran, 0);
    UNTOUCHED("standby not called", fake(ctrl)->standby_count, 0);
    UNTOUCHED("IRQ flags not cleared", fake(ctrl)->clear_irq_count, 0);
    UNTOUCHED("RX callback not detached", fake(ctrl)->clear_callback_count, 0);
    UNTOUCHED("scanChannel not called", fake(ctrl)->scan_count, 0);
    UNTOUCHED("RX not re-armed", fake(ctrl)->start_receive_count, 0);
    UNTOUCHED("received not set", ctrl->received.load() ? 1 : 0, 0);
#undef UNTOUCHED
}

/* T1: both probe sites, both stages. */
static void test_a_frame_in_progress_is_left_alone(void)
{
    static const struct {
        const char *name;
        RadioRxProgress progress;
        bool try_probe;
    } cases[] = {
        { "managed probe, preamble", RADIO_RX_PROGRESS_PREAMBLE, false },
        { "managed probe, header", RADIO_RX_PROGRESS_HEADER, false },
        { "GET CHANNEL probe, preamble", RADIO_RX_PROGRESS_PREAMBLE, true },
        { "GET CHANNEL probe, header", RADIO_RX_PROGRESS_HEADER, true },
    };

    for (const auto &c : cases) {
        RadioController ctrl;

        init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
        fake(&ctrl)->scan_result = 0;   /* a scan would say FREE */
        fake(&ctrl)->rx_progress = c.progress;

        RadioCadProbeResult r = c.try_probe ? radio_cad_try_probe(&ctrl)
                                            : radio_cad_probe(&ctrl);
        expect_receiver_untouched(c.name, &ctrl, r);
    }
}

/* T1 through the MANAGED wait: a frame arriving all through the wait blocks
 * the TX and is never cut off. */
static void test_managed_wait_frame_in_progress_blocks(void)
{
    RadioController ctrl;
    DataTxDaemonContext tx;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.tx_mode = RADIO_TX_MODE_MANAGED;
    tx.ctrl = &ctrl;
    tx.log_ctx = "TEST";
    fake(&ctrl)->scan_result = 0;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_HEADER;

    expect_int("managed wait frame in progress blocks",
               data_tx_wait_channel_free_with_limits_ex(&tx, 3, 1, 0, false),
               DATA_TX_CAD_WAIT_BLOCK);
    expect_int("managed wait frame in progress: standby never called",
               fake(&ctrl)->standby_count, 0);
    expect_int("managed wait frame in progress: never scanned",
               fake(&ctrl)->scan_count, 0);
}

/* Age the current sighting by `ms` (the probe records it on first sight). */
static void age_sighting(RadioController *ctrl, int64_t ms)
{
    ctrl->cad_rx_seen_ms -= ms;
}

/* The probe after the bound runs normally: standby, scan, restore. */
static void expect_normal_probe(const char *name, RadioController *ctrl,
                                const RadioCadProbeResult &r, int scans)
{
    char n[160];

    snprintf(n, sizeof(n), "%s: scanned", name);
    expect_int(n, r.scan_ran, 1);
    snprintf(n, sizeof(n), "%s: scan count", name);
    expect_int(n, fake(ctrl)->scan_count, scans);
    snprintf(n, sizeof(n), "%s: FREE from the scan", name);
    expect_int(n, r.status, RADIO_CAD_PROBE_FREE);
}

/* T2 (a): a latched header flag with no RxDone is trusted for ToA(255 B),
 * then the scan decides, and its re-arm clears the latch: no wedge.
 * Residual: at high SF ToA(255 B) exceeds CADWAIT (~10 s at SF12 against
 * 1.5 s), so a stale header flag there ends the waiting TX as CHANNEL_BUSY
 * (a dropped TX, not a cut frame), as a real frame of that length does. */
static void test_a_stale_header_flag_is_bounded(void)
{
    RadioController ctrl;
    RadioCadProbeResult r;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_HEADER;

    r = radio_cad_probe(&ctrl);
    expect_int("stale header: first sighting BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    expect_int("stale header: bound is the 255 B airtime",
               (int)fake(&ctrl)->last_airtime_len, 255);
    age_sighting(&ctrl, 25000);
    r = radio_cad_probe(&ctrl);
    expect_int("stale header: inside 25.5 s still BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    expect_int("stale header: inside 25.5 s no standby", fake(&ctrl)->standby_count, 0);
    age_sighting(&ctrl, 1000);
    r = radio_cad_probe(&ctrl);
    expect_normal_probe("stale header past 25.5 s", &ctrl, r, 1);
    expect_int("stale header: the scan's restore cleared the latch",
               fake(&ctrl)->rx_progress, RADIO_RX_PROGRESS_NONE);
    r = radio_cad_probe(&ctrl);
    expect_normal_probe("stale header: next probe", &ctrl, r, 2);
}

/* T2 (a'): a preamble on noise, no header following, is released at the
 * stage (i) bound ToA(4 B) = 400 ms here, not at ToA(255 B). */
static void test_a_preamble_without_header_is_released_early(void)
{
    RadioController ctrl;
    RadioCadProbeResult r;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;

    r = radio_cad_probe(&ctrl);
    expect_int("noise preamble: first sighting BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    expect_int("noise preamble: bound is the 4 B airtime",
               (int)fake(&ctrl)->last_airtime_len, 4);
    age_sighting(&ctrl, 1000);
    r = radio_cad_probe(&ctrl);
    expect_normal_probe("noise preamble past 400 ms", &ctrl, r, 1);
}

/* A packet that completes while a stale sighting runs out keeps the
 * pending-packet path: BUSY, `received` set, the IRQ flags left for the
 * drain (never cleared by the probe). */
static void test_a_packet_pending_at_the_bound_is_kept(void)
{
    RadioController ctrl;
    RadioCadProbeResult r;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;
    r = radio_cad_probe(&ctrl);
    expect_int("pending at the bound: stale preamble BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    age_sighting(&ctrl, 1000);
    fake(&ctrl)->rx_done_on_standby = true;   /* RxDone is latched at the quiesce */
    r = radio_cad_probe(&ctrl);
    expect_int("pending at the bound: BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    expect_int("pending at the bound: received set", ctrl.received.load() ? 1 : 0, 1);
    expect_int("pending at the bound: IRQ flags not cleared", fake(&ctrl)->clear_irq_count, 0);
    expect_int("pending at the bound: no scan", fake(&ctrl)->scan_count, 0);
    expect_int("pending at the bound: not re-armed", fake(&ctrl)->start_receive_count, 0);
}

/* A sighting belongs to one frame. Frame 1 is seen, completes (RxDone
 * pending) and is drained; frame 2 starts right behind it, 50 ms later. It
 * must get its own sighting, not frame 1's stamp, which is past the bound by
 * then -- or the probe cuts it. Both probe sites; and the end of frame 1
 * through a successful RX re-arm alone (the drain's, without the probe
 * seeing the pending packet). */
static void test_the_next_frame_gets_its_own_sighting(void)
{
    for (int site = 0; site < 3; site++) {
        static const char *names[] = { "managed probe", "GET CHANNEL probe", "re-arm" };
        RadioController ctrl;
        RadioCadProbeResult r;
        char n[128];

        init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
        fake(&ctrl)->scan_result = 0;
        fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_HEADER;
        r = site == 1 ? radio_cad_try_probe(&ctrl) : radio_cad_probe(&ctrl);
        snprintf(n, sizeof(n), "next frame (%s): frame 1 BUSY", names[site]);
        expect_int(n, r.status, RADIO_CAD_PROBE_BUSY);
        age_sighting(&ctrl, 600);
        fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_NONE;   /* frame 1 complete */
        if (site < 2) {
            ctrl.received.store(true);
            r = site == 1 ? radio_cad_try_probe(&ctrl) : radio_cad_probe(&ctrl);
            snprintf(n, sizeof(n), "next frame (%s): frame 1 RxDone pending BUSY", names[site]);
            expect_int(n, r.status, RADIO_CAD_PROBE_BUSY);
            ctrl.received.store(false);                       /* drained */
        } else {
            daemon_rx_rearm_note_result(&ctrl, 0, "RX restart");
        }
        fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;   /* frame 2 */
        age_sighting(&ctrl, 50);
        r = site == 1 ? radio_cad_try_probe(&ctrl) : radio_cad_probe(&ctrl);
        snprintf(n, sizeof(n), "next frame (%s): frame 2 BUSY", names[site]);
        expect_int(n, r.status, RADIO_CAD_PROBE_BUSY);
        snprintf(n, sizeof(n), "next frame (%s): frame 2 not cut (no standby)", names[site]);
        expect_int(n, fake(&ctrl)->standby_count, 0);
    }
}

/* The bound runs on the radio's clock, not per TX job: a stale header flag
 * (SF12-like, ToA(255 B) far beyond CADWAIT) ends job 1 as BLOCK
 * (CHANNEL_BUSY); job 2, after the bound, gets the normal probe and goes. */
static void test_the_bound_outlives_a_tx_job(void)
{
    RadioController ctrl;
    DataTxDaemonContext tx;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    ctrl.tx_mode = RADIO_TX_MODE_MANAGED;
    tx.ctrl = &ctrl;
    tx.log_ctx = "TEST";
    fake(&ctrl)->scan_result = 0;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_HEADER;   /* latched, no RxDone */

    expect_int("stale header, job 1: CHANNEL_BUSY",
               data_tx_wait_channel_free_with_limits_ex(&tx, 3, 1, 0, false),
               DATA_TX_CAD_WAIT_BLOCK);
    expect_int("stale header, job 1: never scanned", fake(&ctrl)->scan_count, 0);
    age_sighting(&ctrl, 26000);
    expect_int("stale header, job 2 past the bound: FREE",
               data_tx_wait_channel_free_with_limits_ex(&tx, 3, 1, 0, false),
               DATA_TX_CAD_WAIT_FREE);
    expect_int("stale header, job 2: the scan decided", fake(&ctrl)->scan_count, 1);
}

/* A header after the preamble raises the bound to ToA(255 B), counted from
 * the first (preamble) sighting. */
static void test_a_header_after_the_preamble_extends_the_bound(void)
{
    RadioController ctrl;
    RadioCadProbeResult r;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;

    r = radio_cad_probe(&ctrl);
    expect_int("preamble then header: preamble BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_HEADER;
    age_sighting(&ctrl, 1000);
    r = radio_cad_probe(&ctrl);
    expect_int("preamble then header: header 1 s on still BUSY", r.status,
               RADIO_CAD_PROBE_BUSY);
    expect_int("preamble then header: no standby", fake(&ctrl)->standby_count, 0);
}

/* A probe that reads no frame forgets the old sighting: the next frame gets
 * its full bound. */
static void test_a_quiet_reading_starts_a_new_sighting(void)
{
    RadioController ctrl;
    RadioCadProbeResult r;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;
    fake(&ctrl)->rx_progress_latched = false;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;

    r = radio_cad_probe(&ctrl);
    expect_int("new sighting: first frame BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    age_sighting(&ctrl, 1000);
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_NONE;
    r = radio_cad_probe(&ctrl);
    expect_normal_probe("new sighting: quiet in between", &ctrl, r, 1);
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;
    r = radio_cad_probe(&ctrl);
    expect_int("new sighting: next frame BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    expect_int("new sighting: next frame no standby", fake(&ctrl)->standby_count, 1);
}

/* A quiet reading ends the sighting by itself, also when the probe that
 * read it re-arms nothing: here the radio will not stop (UNAVAILABLE, no
 * scan, no restore). The next frame still gets its own bound. */
static void test_a_quiet_reading_ends_the_sighting_without_a_rearm(void)
{
    RadioController ctrl;
    RadioCadProbeResult r;

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_HEADER;
    r = radio_cad_probe(&ctrl);
    expect_int("quiet without re-arm: frame 1 BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    age_sighting(&ctrl, 600);
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_NONE;
    fake(&ctrl)->standby_result = -1;
    r = radio_cad_probe(&ctrl);
    expect_int("quiet without re-arm: radio will not stop, UNAVAILABLE", r.status,
               RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("quiet without re-arm: nothing re-armed", fake(&ctrl)->start_receive_count, 0);
    fake(&ctrl)->standby_result = 0;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;
    age_sighting(&ctrl, 50);
    r = radio_cad_probe(&ctrl);
    expect_int("quiet without re-arm: frame 2 BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    expect_int("quiet without re-arm: frame 2 not cut", fake(&ctrl)->standby_count, 1);
}

/* --- The MANAGED TX loop itself (daemon_tx_executor), CADIDLE 5 ticks ---- */

/* One poll of the managed loop: the clock moves on by the poll interval (the
 * sighting ages by it), and `each_tick` may change what the chip shows. */
struct ManagedRun {
    RadioController *ctrl;
    int ticks;
    int sends;
    void (*each_tick)(ManagedRun *run);
};

static void managed_sleep(uint32_t usec, void *ctx)
{
    ManagedRun *run = (ManagedRun *)ctx;

    run->ticks++;
    run->ctrl->cad_rx_seen_ms -= (int64_t)(usec / 1000u);
    if (run->each_tick)
        run->each_tick(run);
}

static TxResult managed_send(uint8_t *, size_t, int, void *ctx)
{
    ((ManagedRun *)ctx)->sends++;
    return TX_RESULT_OK;
}

/* CADWAIT 1500 / CADIDLE 250 / CADPOLL 50 ms, the stored bench values:
 * 30 polls at most, 5 FREE polls in a row to key. */
static DaemonTxJobResult run_managed_job(ManagedRun *run)
{
    DaemonTxJob job;
    static const uint8_t payload[1] = { 0x42 };

    daemon_tx_job_init(&job, radio_controller_band_number(run->ctrl), RADIO_TX_MODE_MANAGED, 1);
    daemon_tx_job_set_payload(&job, payload, sizeof(payload));
    daemon_tx_job_configure_cad_policy(&job, 1, 30, 5, 50000, 0);
    return daemon_tx_execute_job_with_sender_and_cad(&job, managed_send, run,
                                                    daemon_data_tx_worker_cad_probe, run->ctrl,
                                                    managed_sleep, run);
}

/* T2 (c): a live status stuck true on an idle (FREE) channel must not wedge
 * the TX: it keys within CADWAIT, as on 1.7.1. (Bench S2: 20/20 CAD_TIMEOUT.) */
static void test_a_stuck_live_status_does_not_wedge_the_managed_tx(void)
{
    RadioController ctrl;
    ManagedRun run = { &ctrl, 0, 0, NULL };

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;   /* the scan says FREE */
    fake(&ctrl)->rx_progress_latched = false;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;

    DaemonTxJobResult r = run_managed_job(&run);
    expect_int("stuck live status, managed TX: keys (OK)", r.outcome, DAEMON_TX_OUTCOME_OK);
    expect_int("stuck live status, managed TX: sent once", run.sends, 1);
}

/* T2 (b), the two cases the bound tells apart. A frame that keeps arriving
 * (status true probe after probe, inside its bound, then RxDone) is never
 * scanned. Noise that flaps (true, false, true, ...) gets a scan at every
 * false reading -- a true reading cannot be told from a frame start, so it
 * stays BUSY (stated limit). */
static void test_an_ongoing_frame_and_flapping_noise(void)
{
    {
        RadioController ctrl;
        RadioCadProbeResult r;

        init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
        fake(&ctrl)->scan_result = 0;
        fake(&ctrl)->rx_progress_latched = false;
        fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;
        r = radio_cad_probe(&ctrl);
        expect_int("ongoing frame: preamble BUSY", r.status, RADIO_CAD_PROBE_BUSY);
        age_sighting(&ctrl, 300);
        r = radio_cad_probe(&ctrl);
        expect_int("ongoing frame: preamble 300 ms BUSY", r.status, RADIO_CAD_PROBE_BUSY);
        fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_HEADER;
        for (int i = 0; i < 3; i++) {
            age_sighting(&ctrl, 5000);
            r = radio_cad_probe(&ctrl);
            expect_int("ongoing frame: payload BUSY", r.status, RADIO_CAD_PROBE_BUSY);
        }
        expect_int("ongoing frame: never scanned", fake(&ctrl)->scan_count, 0);
        expect_int("ongoing frame: never stopped", fake(&ctrl)->standby_count, 0);
    }
    {
        RadioController ctrl;
        RadioCadProbeResult r;
        int busy = 0, scans = 0;

        init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
        fake(&ctrl)->scan_result = 0;
        fake(&ctrl)->rx_progress_latched = false;
        for (int i = 0; i < 6; i++) {
            fake(&ctrl)->rx_progress = (i % 2) ? RADIO_RX_PROGRESS_NONE
                                               : RADIO_RX_PROGRESS_PREAMBLE;
            r = radio_cad_probe(&ctrl);
            busy += r.status == RADIO_CAD_PROBE_BUSY;
            scans += r.scan_ran;
        }
        expect_int("flapping noise: BUSY on each true reading", busy, 3);
        expect_int("flapping noise: a scan on each false reading", scans, 3);
    }
}

/* T2 (b) through the MANAGED loop: noise that flaps true / NONE at every
 * poll restarts the sighting at each true reading (BUSY, no scan), so the
 * loop never sees CADIDLE FREE polls in a row and the TX ends CHANNEL_BUSY
 * at CADWAIT with nothing sent -- the stated limit, held: the wait ends in
 * bounded time, nothing is sent over a possible frame start and no frame is
 * cut (LBT as on 1.7.1). That a TX on an idle channel makes progress is the
 * on-air bench's proof, not this test's. */
static void flap_live_status(ManagedRun *run)
{
    FakeRadio *f = fake(run->ctrl);

    f->rx_progress = f->rx_progress == RADIO_RX_PROGRESS_NONE
                         ? RADIO_RX_PROGRESS_PREAMBLE
                         : RADIO_RX_PROGRESS_NONE;
}

static void test_flapping_noise_through_the_managed_tx(void)
{
    RadioController ctrl;
    ManagedRun run = { &ctrl, 0, 0, flap_live_status };

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;   /* the scan says FREE */
    fake(&ctrl)->rx_progress_latched = false;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;

    DaemonTxJobResult r = run_managed_job(&run);
    expect_int("flapping noise, managed TX: CHANNEL_BUSY", r.outcome,
               DAEMON_TX_OUTCOME_CHANNEL_BUSY);
    expect_int("flapping noise, managed TX: nothing sent", run.sends, 0);
    expect_int("flapping noise, managed TX: scans ran", fake(&ctrl)->scan_count > 0, 1);
    expect_int("flapping noise, managed TX: waited to CADWAIT", run.ticks >= 29, 1);
}

/* T2 (c)/(e): a LIVE status stuck true. Once the bound has run out, the scan
 * decides at every probe -- also after the scan's own re-arms -- until the
 * status reads NONE once; the next frame is then protected again (BUSY, no
 * standby). Without the hold-off, every re-arm started a new sighting and
 * the probes alternated BUSY / FREE. */
static void test_a_stuck_live_status_is_held_off_until_a_quiet_reading(void)
{
    RadioController ctrl;
    RadioCadProbeResult r;
    char n[96];

    init_ctrl(&ctrl, RADIO_HEALTH_READY, RADIO_MODE_LORA);
    fake(&ctrl)->scan_result = 0;
    fake(&ctrl)->rx_progress_latched = false;
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;

    r = radio_cad_probe(&ctrl);
    expect_int("stuck live status: BUSY inside the bound", r.status, RADIO_CAD_PROBE_BUSY);
    age_sighting(&ctrl, 1000);
    for (int i = 1; i <= 3; i++) {
        r = radio_cad_probe(&ctrl);
        snprintf(n, sizeof(n), "stuck live status, held off, probe %d", i);
        expect_normal_probe(n, &ctrl, r, i);
    }
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_NONE;
    r = radio_cad_probe(&ctrl);
    expect_normal_probe("stuck live status: quiet reading", &ctrl, r, 4);
    fake(&ctrl)->rx_progress = RADIO_RX_PROGRESS_PREAMBLE;
    const int standby_before = fake(&ctrl)->standby_count;
    r = radio_cad_probe(&ctrl);
    expect_int("after the quiet reading: a new frame is BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    expect_int("after the quiet reading: a new frame is not cut",
               fake(&ctrl)->standby_count, standby_before);
}

int main(int argc, char **argv)
{
    /* daemon_data_tx_context() reads log tags from the band descriptor. */
    daemon_band_resolve(RADIO_BAND_433);

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
    test_probe_pending_rx_returns_busy();
    test_managed_wait_pending_rx_blocks();
    test_tx_wait_fsk_skips_cad();
    test_passive_probe_is_non_destructive();
    test_passive_probe_uses_per_band_threshold();
    test_passive_probe_non_lora_unavailable();
    test_passive_probe_during_rx_rearm_unavailable();
    test_active_probe_during_rx_rearm_does_not_scan();
    test_probe_detaches_rx_callback_around_the_scan();
    test_active_probe_in_fsk_uses_only_the_nondestructive_read();
    test_active_probe_during_rearm_makes_no_radio_call_at_all();
    test_a_packet_completing_during_the_quiesce_is_preserved();
    test_a_radio_that_will_not_quiesce_is_unavailable();
    test_restore_clears_received_and_irq();
    test_active_probe_leaves_no_spurious_received();
    test_a_frame_in_progress_is_left_alone();
    test_managed_wait_frame_in_progress_blocks();
    test_a_stale_header_flag_is_bounded();
    test_a_preamble_without_header_is_released_early();
    test_a_header_after_the_preamble_extends_the_bound();
    test_a_packet_pending_at_the_bound_is_kept();
    test_the_bound_outlives_a_tx_job();
    test_the_next_frame_gets_its_own_sighting();
    test_a_quiet_reading_starts_a_new_sighting();
    test_a_quiet_reading_ends_the_sighting_without_a_rearm();
    test_a_stuck_live_status_does_not_wedge_the_managed_tx();
    test_an_ongoing_frame_and_flapping_noise();
    test_flapping_noise_through_the_managed_tx();
    test_a_stuck_live_status_is_held_off_until_a_quiet_reading();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
