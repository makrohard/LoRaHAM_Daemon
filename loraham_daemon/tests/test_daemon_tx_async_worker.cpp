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

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
