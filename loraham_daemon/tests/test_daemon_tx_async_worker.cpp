#include "../daemon_tx_async_worker.h"

#include <chrono>
#include <stdio.h>
#include <string.h>
#include <thread>

/* --- TX async worker tests ---------------------------------------------- */

static int g_ok = 0;
static int g_fail = 0;

TxResult lora_send(uint8_t *buf, size_t len, int band)
{
    (void)buf;
    (void)len;
    (void)band;
    return TX_RESULT_RADIO_ERROR;
}

typedef struct {
    int calls;
    TxResult result[8];
} FakeSender;

typedef struct {
    int calls;
    DaemonTxJobResult last;
} ResultRecorder;

typedef struct {
    int calls;
    int busy_before_free;
    int always_busy;
    int sleep_calls;
} FakeCad;

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
    (void)len;
    (void)band;

    sender->calls++;

    return sender->result[idx];
}

static void record_result(const DaemonTxJobResult *result, void *ctx)
{
    ResultRecorder *rec = (ResultRecorder *)ctx;

    if (!rec || !result)
        return;

    rec->calls++;
    rec->last = *result;
}

static int fake_cad_probe(int band, void *ctx)
{
    FakeCad *cad = (FakeCad *)ctx;

    (void)band;

    cad->calls++;
    if (cad->always_busy)
        return DAEMON_TX_CAD_PROBE_BUSY;

    return cad->calls <= cad->busy_before_free
        ? DAEMON_TX_CAD_PROBE_BUSY
        : DAEMON_TX_CAD_PROBE_FREE;
}

static void fake_cad_sleep(uint32_t usec, void *ctx)
{
    FakeCad *cad = (FakeCad *)ctx;

    (void)usec;

    cad->sleep_calls++;
}

static DaemonTxJob make_job(uint16_t seq)
{
    DaemonTxJob job;
    uint8_t payload[] = { 0x42 };

    daemon_tx_job_init(&job, 433, 0, seq);
    daemon_tx_job_set_payload(&job, payload, sizeof(payload));

    return job;
}

static int wait_processed(DaemonTxAsyncWorker *async, size_t expected)
{
    for (int i = 0; i < 1000; i++) {
        if (daemon_tx_async_worker_processed(async) >= expected)
            return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return 0;
}

static void test_start_submit_stop(void)
{
    DaemonTxAsyncWorker async;
    FakeSender sender;
    ResultRecorder recorder;
    DaemonTxJob job = make_job(7);

    memset(&sender, 0, sizeof(sender));
    memset(&recorder, 0, sizeof(recorder));
    sender.result[0] = TX_RESULT_OK;

    daemon_tx_async_worker_init(&async);
    daemon_tx_async_worker_configure(&async,
                                     fake_send,
                                     &sender,
                                     record_result,
                                     &recorder);

    expect_int("async start", daemon_tx_async_worker_start(&async), 0);
    expect_int("async running", daemon_tx_async_worker_running(&async), 1);
    expect_int("async submit", daemon_tx_async_worker_submit(&async, &job), 0);
    expect_int("async processed wait", wait_processed(&async, 1), 1);

    daemon_tx_async_worker_stop(&async);

    expect_int("async stopped", daemon_tx_async_worker_running(&async), 0);
    expect_int("async sender calls", sender.calls, 1);
    expect_int("async result callback", recorder.calls, 1);
    expect_int("async result seq", recorder.last.seq, 7);
    expect_size("async pending empty", daemon_tx_async_worker_pending(&async), 0);
}

static void test_full_queue_rejects(void)
{
    DaemonTxAsyncWorker async;

    daemon_tx_async_worker_init(&async);

    for (int i = 0; i < DAEMON_TX_QUEUE_CAPACITY; i++) {
        DaemonTxJob job = make_job((uint16_t)i);
        expect_int("async fill submit", daemon_tx_async_worker_submit(&async, &job), 0);
    }

    DaemonTxJob extra = make_job(99);
    expect_int("async full reject", daemon_tx_async_worker_submit(&async, &extra), -1);
    expect_size("async rejected count", daemon_tx_async_worker_rejected(&async), 1);
    expect_size("async dropped count", daemon_tx_async_worker_dropped(&async), 1);
    expect_size("async pending capacity", daemon_tx_async_worker_pending(&async),
                DAEMON_TX_QUEUE_CAPACITY);
}

static void test_stop_drains_existing_jobs(void)
{
    DaemonTxAsyncWorker async;
    FakeSender sender;
    ResultRecorder recorder;
    DaemonTxJob a = make_job(10);
    DaemonTxJob b = make_job(11);

    memset(&sender, 0, sizeof(sender));
    memset(&recorder, 0, sizeof(recorder));
    sender.result[0] = TX_RESULT_OK;
    sender.result[1] = TX_RESULT_OK;

    daemon_tx_async_worker_init(&async);
    daemon_tx_async_worker_configure(&async,
                                     fake_send,
                                     &sender,
                                     record_result,
                                     &recorder);

    expect_int("async start drain", daemon_tx_async_worker_start(&async), 0);
    expect_int("async submit a", daemon_tx_async_worker_submit(&async, &a), 0);
    expect_int("async submit b", daemon_tx_async_worker_submit(&async, &b), 0);

    daemon_tx_async_worker_stop(&async);

    expect_int("async drain sender calls", sender.calls, 2);
    expect_size("async drain processed", daemon_tx_async_worker_processed(&async), 2);
    expect_size("async drain pending", daemon_tx_async_worker_pending(&async), 0);
}

static void test_worker_cad_waits_before_send(void)
{
    DaemonTxAsyncWorker async;
    FakeSender sender;
    FakeCad cad;
    ResultRecorder recorder;
    DaemonTxJob job = make_job(20);

    memset(&sender, 0, sizeof(sender));
    memset(&cad, 0, sizeof(cad));
    memset(&recorder, 0, sizeof(recorder));

    cad.busy_before_free = 2;
    sender.result[0] = TX_RESULT_OK;
    daemon_tx_job_configure_cad_policy(&job, 1, 5, 1, 100, 0);

    daemon_tx_async_worker_init(&async);
    daemon_tx_async_worker_configure(&async,
                                     fake_send,
                                     &sender,
                                     record_result,
                                     &recorder);
    daemon_tx_async_worker_configure_cad(&async,
                                         fake_cad_probe,
                                         &cad,
                                         fake_cad_sleep,
                                         &cad);

    expect_int("async cad start", daemon_tx_async_worker_start(&async), 0);
    expect_int("async cad submit", daemon_tx_async_worker_submit(&async, &job), 0);
    expect_int("async cad processed wait", wait_processed(&async, 1), 1);

    daemon_tx_async_worker_stop(&async);

    expect_int("async cad probe calls", cad.calls, 3);
    expect_int("async cad sleeps", cad.sleep_calls, 2);
    expect_int("async cad sender calls", sender.calls, 1);
    expect_int("async cad result ok", recorder.last.tx_result, TX_RESULT_OK);
}

static void test_worker_cad_timeout_blocks_without_send(void)
{
    DaemonTxAsyncWorker async;
    FakeSender sender;
    FakeCad cad;
    ResultRecorder recorder;
    DaemonTxJob job = make_job(21);

    memset(&sender, 0, sizeof(sender));
    memset(&cad, 0, sizeof(cad));
    memset(&recorder, 0, sizeof(recorder));

    cad.always_busy = 1;
    daemon_tx_job_configure_cad_policy(&job, 1, 2, 1, 100, 0);

    daemon_tx_async_worker_init(&async);
    daemon_tx_async_worker_configure(&async,
                                     fake_send,
                                     &sender,
                                     record_result,
                                     &recorder);
    daemon_tx_async_worker_configure_cad(&async,
                                         fake_cad_probe,
                                         &cad,
                                         fake_cad_sleep,
                                         &cad);

    expect_int("async cad block start", daemon_tx_async_worker_start(&async), 0);
    expect_int("async cad block submit", daemon_tx_async_worker_submit(&async, &job), 0);
    expect_int("async cad block processed wait", wait_processed(&async, 1), 1);

    daemon_tx_async_worker_stop(&async);

    expect_int("async cad block probe calls", cad.calls, 2);
    expect_int("async cad block sleeps", cad.sleep_calls, 1);
    expect_int("async cad block sender calls", sender.calls, 0);
    expect_int("async cad block result", recorder.last.tx_result, TX_RESULT_CAD_TIMEOUT);
    expect_int("async cad block framed", recorder.last.framed_status,
               FRAMED_DATA_TX_STATUS_CHANNEL_BUSY);
}

static void test_worker_cad_timeout_sends_with_flag(void)
{
    DaemonTxAsyncWorker async;
    FakeSender sender;
    FakeCad cad;
    ResultRecorder recorder;
    DaemonTxJob job = make_job(22);

    memset(&sender, 0, sizeof(sender));
    memset(&cad, 0, sizeof(cad));
    memset(&recorder, 0, sizeof(recorder));

    cad.always_busy = 1;
    sender.result[0] = TX_RESULT_OK;
    daemon_tx_job_configure_cad_policy(&job, 1, 2, 1, 100, 1);

    daemon_tx_async_worker_init(&async);
    daemon_tx_async_worker_configure(&async,
                                     fake_send,
                                     &sender,
                                     record_result,
                                     &recorder);
    daemon_tx_async_worker_configure_cad(&async,
                                         fake_cad_probe,
                                         &cad,
                                         fake_cad_sleep,
                                         &cad);

    expect_int("async cad timeout send start", daemon_tx_async_worker_start(&async), 0);
    expect_int("async cad timeout send submit", daemon_tx_async_worker_submit(&async, &job), 0);
    expect_int("async cad timeout send processed wait", wait_processed(&async, 1), 1);

    daemon_tx_async_worker_stop(&async);

    expect_int("async cad timeout send probes", cad.calls, 2);
    expect_int("async cad timeout send sleeps", cad.sleep_calls, 1);
    expect_int("async cad timeout sender calls", sender.calls, 1);
    expect_int("async cad timeout result", recorder.last.tx_result, TX_RESULT_OK);
    expect_int("async cad timeout flag",
               recorder.last.flags & FRAMED_DATA_TX_RESULT_FLAG_CAD_TIMEOUT,
               FRAMED_DATA_TX_RESULT_FLAG_CAD_TIMEOUT);
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

    test_start_submit_stop();
    test_full_queue_rejects();
    test_stop_drains_existing_jobs();
    test_worker_cad_waits_before_send();
    test_worker_cad_timeout_blocks_without_send();
    test_worker_cad_timeout_sends_with_flag();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
