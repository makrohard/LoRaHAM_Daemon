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
    int scan_state;
    float rssi;

    FakeRadio() : scan_count(0), scan_state(0), rssi(-81.0f) {}

    int scanChannel()
    {
        scan_count++;
        return scan_state;
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
    ctx->completion_slot = DAEMON_TX_COMPLETION_SLOT_NONE;
    ctx->completion_seq = 0;
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
    ctx.completion_slot = 3;
    ctx.completion_seq = 44;

    expect_int("queue tx result",
               send_data_chunk<FakeRadio>(payload, sizeof(payload), 0, &ctx),
               0);
    expect_int("queue processed wait", wait_async_processed(433, 1), 1);
    expect_int("queue sender calls", sender.calls, 1);
    expect_size("queue async accepted", daemon_tx_async_runtime_accepted_for_band(433), 1);
    expect_size("queue async processed", daemon_tx_async_runtime_processed_for_band(433), 1);
    expect_size("queue async pending drained", daemon_tx_async_runtime_pending_for_band(433), 0);

    DaemonTxJobResult last;
    expect_int("queue async last present",
               daemon_tx_async_runtime_last_result_for_band(433, &last),
               1);
    expect_int("queue async last result", last.tx_result, TX_RESULT_OK);

    expect_size("queue completion pending",
                daemon_tx_async_runtime_completion_pending_for_band(433),
                1);
    DaemonTxJobResult completion;
    expect_int("queue completion pop",
               daemon_tx_async_runtime_pop_completion_for_band(433, &completion),
               0);
    expect_int("queue completion result", completion.tx_result, TX_RESULT_OK);
    expect_int("queue completion target", completion.completion_slot, 3);
    expect_int("queue completion seq", completion.seq, 44);
    expect_size("queue completion pending drained",
                daemon_tx_async_runtime_completion_pending_for_band(433),
                0);
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


static void test_raw_busy_probe_blocks_immediately(void)
{
    RadioController<FakeRadio> ctrl;
    DataTxDaemonContext<FakeRadio> ctx;
    FakeSender sender;
    uint8_t payload[] = { 9 };

    init_context(&ctrl, &ctx, &sender);
    ctrl.mode = RADIO_MODE_LORA;
    ctrl.tx_mode = RADIO_TX_MODE_RAW;
    ctrl.radio->scan_state = 1;

    expect_int("raw busy tx result",
               send_data_chunk<FakeRadio>(payload, sizeof(payload), 0, &ctx),
               DAEMON_TX_OUTCOME_CHANNEL_BUSY);
    expect_int("raw busy no send", sender.calls, 0);
    expect_int("raw busy one cad probe", ctrl.radio->scan_count, 1);
}

static void test_managed_busy_timeout_policy_sends_anyway(void)
{
    RadioController<FakeRadio> ctrl;
    DataTxDaemonContext<FakeRadio> ctx;
    FakeSender sender;

    init_context(&ctrl, &ctx, &sender);
    ctrl.mode = RADIO_MODE_LORA;
    ctrl.tx_mode = RADIO_TX_MODE_MANAGED;
    ctrl.radio->scan_state = 1;

    expect_int("managed busy timeout sends anyway",
               data_tx_wait_channel_free_with_limits<FakeRadio>(&ctx, 2, 0),
               0);
    expect_int("managed busy timeout probe count", ctrl.radio->scan_count, 2);
}

static void test_managed_free_sends_immediately(void)
{
    RadioController<FakeRadio> ctrl;
    DataTxDaemonContext<FakeRadio> ctx;
    FakeSender sender;

    init_context(&ctrl, &ctx, &sender);
    ctrl.mode = RADIO_MODE_LORA;
    ctrl.tx_mode = RADIO_TX_MODE_MANAGED;
    ctrl.radio->scan_state = 0;

    expect_int("managed free sends",
               data_tx_wait_channel_free_with_limits<FakeRadio>(&ctx, 2, 0),
               0);
    expect_int("managed free one probe", ctrl.radio->scan_count, 1);
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
    test_raw_busy_probe_blocks_immediately();
    test_managed_busy_timeout_policy_sends_anyway();
    test_managed_free_sends_immediately();
    test_not_ready_still_short_circuits();

    daemon_tx_async_runtime_shutdown();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
