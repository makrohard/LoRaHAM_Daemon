#include "../daemon_rx_rearm.h"

#include <stdio.h>

#include <condition_variable>
#include <mutex>
#include <thread>

/* --- RX re-arm robustness tests (audit M3) -------------------------------- */
/*
 * Covers the fail-closed RX-arm contract: boot failure flips health to
 * FAILED; a failed runtime re-arm latches rx_rearm_pending and counts in
 * rx_rearm_failures; the main-loop retry re-arms under try_to_lock (skipped
 * while tx_busy or on lock contention) and clears the latch on success.
 */

static int g_ok = 0;
static int g_fail = 0;

struct FakeRadio : public RadioDriver {
    int16_t start_receive_result;
    int start_receive_count;
    int callback_count;
    void (*last_callback)(void);

    FakeRadio() : RadioDriver(NULL),
                  start_receive_result(0), start_receive_count(0),
                  callback_count(0), last_callback(NULL) {}

    void setPacketReceivedAction(void (*cb)(void)) override
    {
        last_callback = cb;
        callback_count++;
    }

    int16_t startReceive() override
    {
        start_receive_count++;
        return start_receive_result;
    }

    int16_t clearIrq(uint32_t) override { return 0; }
    int16_t scanChannel() override { return 0; }
    float getRSSI() override { return -120.0f; }
    float rssiProbe() override { return -120.0f; }
    int16_t begin(const RadioRfDefaults *) override { return 0; }
    int16_t switchMode(RadioMode_t,
                       const RadioRfDefaults *) override { return 0; }
    void applyLoraParam(const char *, const std::string &,
                        const std::string &) override {}
    void applyFskParam(const char *, const std::string &,
                       const std::string &) override {}
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

static void fake_rx_callback(void)
{
}

static void expect_int(const char *name, long actual, long expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %ld, got %ld\n", name, expected, actual);
    }
}

static void init_ctrl(RadioController *ctrl)
{
    radio_controller_init(ctrl,
                          RADIO_BAND_433,
                          "TEST",
                          false,
                          fake_rx_callback,
                          13);
    ctrl->driver.reset(new FakeRadio());
    ctrl->health = RADIO_HEALTH_READY;
}

/* --- daemon_rx_rearm_note_result ------------------------------------------ */

static void test_note_failure_latches_and_counts(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl);

    daemon_rx_rearm_note_result(&ctrl, -707, "TEST");
    expect_int("failure sets rx_rearm_pending",
               ctrl.rx_rearm_pending.load() ? 1 : 0, 1);
    expect_int("failure increments RXREARMFAIL",
               (long)ctrl.stats.rx_rearm_failures, 1);

    /* Second failure while latched: counts again, latch unchanged. */
    daemon_rx_rearm_note_result(&ctrl, -707, "TEST");
    expect_int("second failure keeps latch",
               ctrl.rx_rearm_pending.load() ? 1 : 0, 1);
    expect_int("second failure counts again",
               (long)ctrl.stats.rx_rearm_failures, 2);
}

static void test_note_success_clears_latch(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl);

    daemon_rx_rearm_note_result(&ctrl, -707, "TEST");
    daemon_rx_rearm_note_result(&ctrl, 0, "TEST");
    expect_int("success clears rx_rearm_pending",
               ctrl.rx_rearm_pending.load() ? 1 : 0, 0);
    expect_int("success leaves counter",
               (long)ctrl.stats.rx_rearm_failures, 1);

    /* Success without a pending incident is a no-op. */
    daemon_rx_rearm_note_result(&ctrl, 0, "TEST");
    expect_int("idle success stays clear",
               ctrl.rx_rearm_pending.load() ? 1 : 0, 0);
}

/* --- daemon_rx_rearm_boot_result ------------------------------------------ */

static void test_boot_failure_fails_closed(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl);

    expect_int("boot success returns true",
               daemon_rx_rearm_boot_result(&ctrl, 0) ? 1 : 0, 1);
    expect_int("boot success keeps READY",
               ctrl.health == RADIO_HEALTH_READY ? 1 : 0, 1);

    expect_int("boot failure returns false",
               daemon_rx_rearm_boot_result(&ctrl, -707) ? 1 : 0, 0);
    expect_int("boot failure sets FAILED",
               ctrl.health == RADIO_HEALTH_FAILED ? 1 : 0, 1);
}

/* --- daemon_rx_rearm_retry ------------------------------------------------ */

static void test_retry_success_rearms(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl);
    ctrl.rx_rearm_pending.store(true);

    daemon_rx_rearm_retry(&ctrl);
    expect_int("retry calls startReceive",
               fake(&ctrl)->start_receive_count, 1);
    expect_int("retry re-sets RX callback",
               fake(&ctrl)->last_callback == fake_rx_callback ? 1 : 0, 1);
    expect_int("retry success clears latch",
               ctrl.rx_rearm_pending.load() ? 1 : 0, 0);
}

static void test_retry_failure_keeps_latch(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl);
    ctrl.rx_rearm_pending.store(true);
    fake(&ctrl)->start_receive_result = -707;

    daemon_rx_rearm_retry(&ctrl);
    expect_int("failed retry keeps latch",
               ctrl.rx_rearm_pending.load() ? 1 : 0, 1);
    expect_int("failed retry counts",
               (long)ctrl.stats.rx_rearm_failures, 1);

    /* Next tick retries again; recovery clears the latch. */
    fake(&ctrl)->start_receive_result = 0;
    daemon_rx_rearm_retry(&ctrl);
    expect_int("next tick recovers",
               ctrl.rx_rearm_pending.load() ? 1 : 0, 0);
}

static void test_retry_skips_without_incident(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl);

    daemon_rx_rearm_retry(&ctrl);
    expect_int("no incident: no startReceive",
               fake(&ctrl)->start_receive_count, 0);
}

static void test_retry_skips_while_tx_busy(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl);
    ctrl.rx_rearm_pending.store(true);
    ctrl.tx_busy.store(true);

    daemon_rx_rearm_retry(&ctrl);
    expect_int("tx_busy: no startReceive",
               fake(&ctrl)->start_receive_count, 0);
    expect_int("tx_busy: latch stays",
               ctrl.rx_rearm_pending.load() ? 1 : 0, 1);
}

static void test_retry_skips_when_not_ready(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl);
    ctrl.rx_rearm_pending.store(true);
    ctrl.health = RADIO_HEALTH_FAILED;

    daemon_rx_rearm_retry(&ctrl);
    expect_int("not READY: no startReceive",
               fake(&ctrl)->start_receive_count, 0);
}

static void test_retry_skips_while_rx_pending(void)
{
    RadioController ctrl;

    init_ctrl(&ctrl);
    ctrl.rx_rearm_pending.store(true);
    ctrl.received.store(true);

    daemon_rx_rearm_retry(&ctrl);
    expect_int("rx pending: no startReceive",
               fake(&ctrl)->start_receive_count, 0);
    expect_int("rx pending: packet flag untouched",
               ctrl.received.load() ? 1 : 0, 1);
    expect_int("rx pending: latch stays",
               ctrl.rx_rearm_pending.load() ? 1 : 0, 1);
}

static void test_retry_skips_on_lock_contention(void)
{
    RadioController ctrl;
    std::mutex gate;
    std::condition_variable cv;
    bool locked = false;
    bool release = false;

    init_ctrl(&ctrl);
    ctrl.rx_rearm_pending.store(true);

    /* A second thread holds radio_mutex (recursive: same-thread lock would
     * succeed, so the contention must come from another thread). */
    std::thread holder([&]() {
        std::lock_guard<std::recursive_mutex> radio_lock(ctrl.radio_mutex);
        {
            std::lock_guard<std::mutex> g(gate);
            locked = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> g(gate);
        cv.wait(g, [&]() { return release; });
    });

    {
        std::unique_lock<std::mutex> g(gate);
        cv.wait(g, [&]() { return locked; });
    }

    daemon_rx_rearm_retry(&ctrl);
    expect_int("lock contention: tick skipped",
               fake(&ctrl)->start_receive_count, 0);
    expect_int("lock contention: latch stays",
               ctrl.rx_rearm_pending.load() ? 1 : 0, 1);

    {
        std::lock_guard<std::mutex> g(gate);
        release = true;
    }
    cv.notify_all();
    holder.join();
}

int main(void)
{
    test_note_failure_latches_and_counts();
    test_note_success_clears_latch();
    test_boot_failure_fails_closed();
    test_retry_success_rearms();
    test_retry_failure_keeps_latch();
    test_retry_skips_without_incident();
    test_retry_skips_while_tx_busy();
    test_retry_skips_when_not_ready();
    test_retry_skips_while_rx_pending();
    test_retry_skips_on_lock_contention();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail == 0 ? 0 : 1;
}
