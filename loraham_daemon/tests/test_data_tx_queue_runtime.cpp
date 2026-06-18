#include "../daemon_data_tx_runtime.h"

#include <chrono>
#include <stdio.h>
#include <thread>
#include <string.h>

/* --- DATA TX queue opt-in runtime tests --------------------------------- */

static int g_ok = 0;
static int g_fail = 0;


/* --- Local production-symbol stubs for this unit test --- */
TxResult lora_send(uint8_t *buf, size_t len, int band)
{
    (void)buf;
    (void)len;
    (void)band;
    return TX_RESULT_RADIO_ERROR;
}

void daemon_led_init(void)
{
}

int daemon_led_ready(void)
{
    return 1;
}

void daemon_led_set_pin(int pin, int state)
{
    (void)pin;
    (void)state;
}

void daemon_led_flash_pin(int pin)
{
    (void)pin;
}
/* --- End local production-symbol stubs --- */





struct FakeRadio {
    int scan_count;
    float rssi;

    FakeRadio() : scan_count(0), rssi(-81.0f) {}

    int scanChannel()
    {
        scan_count++;
        return 0;
    }

    float getRSSI()
    {
        return rssi;
    }
};

typedef struct {
    int calls;
    size_t len[8];
    int band[8];
    TxResult result[8];
} FakeSender;

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

static void expect_size(const char *name, size_t actual, size_t expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %zu, got %zu\n", name, expected, actual);
    }
}

static TxResult fake_send(uint8_t *payload, size_t len, int band, void *ctx)
{
    FakeSender *sender = (FakeSender *)ctx;
    int idx = sender->calls;

    (void)payload;

    sender->len[idx] = len;
    sender->band[idx] = band;
    sender->calls++;

    return sender->result[idx];
}


static int wait_async_processed(int band, size_t expected)
{
    for (int i = 0; i < 1000; i++) {
        if (daemon_tx_async_runtime_processed_for_band(band) >= expected)
            return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}

static void init_context(RadioController<FakeRadio> *ctrl,
                         DataTxDaemonContext<FakeRadio> *ctx,
                         FakeSender *sender)
{
    radio_controller_init(ctrl,
                          RADIO_BAND_433,
                          "TEST",
                          false,
                          fake_rx_callback,
                          13);
    ctrl->radio.reset(new FakeRadio());
    ctrl->health = RADIO_HEALTH_READY;
    ctrl->mode = RADIO_MODE_FSK;

    daemon_tx_async_runtime_shutdown();
    daemon_tx_async_runtime_init();

    memset(sender, 0, sizeof(*sender));
    sender->result[0] = TX_RESULT_OK;

    ctx->ctrl = ctrl;
    ctx->log_ctx = "TEST";
    ctx->send_fn = fake_send;
    ctx->send_ctx = sender;
}

static void test_default_direct_path(void)
{
    RadioController<FakeRadio> ctrl;
    DataTxDaemonContext<FakeRadio> ctx;
    FakeSender sender;
    uint8_t payload[] = { 1, 2, 3 };

    init_context(&ctrl, &ctx, &sender);

    expect_int("direct tx result",
               send_data_chunk<FakeRadio>(payload, sizeof(payload), 0, &ctx),
               0);
    expect_int("direct sender calls", sender.calls, 1);
    expect_size("direct queue accepted", daemon_tx_worker_accepted(&ctrl.tx_worker), 0);
    expect_size("direct queue processed", daemon_tx_worker_processed(&ctrl.tx_worker), 0);
    expect_size("direct queue pending", daemon_tx_worker_pending(&ctrl.tx_worker), 0);
}

static void test_txqueue_optin_path(void)
{
    RadioController<FakeRadio> ctrl;
    DataTxDaemonContext<FakeRadio> ctx;
    FakeSender sender;
    uint8_t payload[] = { 4, 5, 6, 7 };

    init_context(&ctrl, &ctx, &sender);
    ctrl.tx_queue_active.store(true);

    expect_int("queue tx result",
               send_data_chunk<FakeRadio>(payload, sizeof(payload), 0, &ctx),
               0);
    expect_int("queue processed wait", wait_async_processed(433, 1), 1);
    expect_int("queue sender calls", sender.calls, 1);
    expect_size("queue async accepted", daemon_tx_async_runtime_accepted_for_band(433), 1);
    expect_size("queue async processed", daemon_tx_async_runtime_processed_for_band(433), 1);
    expect_size("queue async pending drained", daemon_tx_async_runtime_pending_for_band(433), 0);
}

static void test_txqueue_direct_full_rejects_newest(void)
{
    DaemonTxAsyncWorker *worker;

    daemon_tx_async_runtime_shutdown();
    daemon_tx_async_runtime_init();

    worker = daemon_tx_async_runtime_worker_for_band(433);

    for (int i = 0; i < DAEMON_TX_QUEUE_CAPACITY; i++) {
        DaemonTxJob job;
        uint8_t queued[] = { (uint8_t)i };

        daemon_tx_job_init(&job, 433, RADIO_TX_MODE_MANAGED, (uint16_t)i);
        daemon_tx_job_set_payload(&job, queued, sizeof(queued));
        daemon_tx_async_worker_submit(worker, &job);
    }

    DaemonTxJob extra;
    uint8_t payload[] = { 8, 9 };
    daemon_tx_job_init(&extra, 433, RADIO_TX_MODE_MANAGED, 99);
    daemon_tx_job_set_payload(&extra, payload, sizeof(payload));

    expect_int("queue full direct reject",
               daemon_tx_async_worker_submit(worker, &extra),
               -1);
    expect_size("queue full rejected", daemon_tx_async_runtime_rejected_for_band(433), 1);
    expect_size("queue full dropped", daemon_tx_async_runtime_dropped_for_band(433), 1);
    expect_size("queue full still pending", daemon_tx_async_runtime_pending_for_band(433),
                DAEMON_TX_QUEUE_CAPACITY);

    daemon_tx_async_runtime_shutdown();
}

static void test_not_ready_still_short_circuits(void)
{
    RadioController<FakeRadio> ctrl;
    DataTxDaemonContext<FakeRadio> ctx;
    FakeSender sender;
    uint8_t payload[] = { 1 };

    init_context(&ctrl, &ctx, &sender);
    ctrl.tx_queue_active.store(true);
    ctrl.health = RADIO_HEALTH_FAILED;

    expect_int("not ready tx result",
               send_data_chunk<FakeRadio>(payload, sizeof(payload), 0, &ctx),
               DAEMON_TX_OUTCOME_RADIO_NOT_READY);
    expect_int("not ready no send", sender.calls, 0);
    expect_size("not ready no queued", daemon_tx_async_runtime_pending_for_band(433), 0);
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

    test_default_direct_path();
    test_txqueue_optin_path();
    test_txqueue_direct_full_rejects_newest();
    test_not_ready_still_short_circuits();

    daemon_tx_async_runtime_shutdown();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
