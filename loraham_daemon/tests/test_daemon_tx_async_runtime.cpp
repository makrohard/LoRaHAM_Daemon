#include "../daemon_tx_async_runtime.h"
#include "../daemon_log.h"

#include <stdio.h>
#include <string.h>

/* --- TX async runtime lifecycle tests ----------------------------------- */

static int g_ok = 0;
static int g_fail = 0;

DaemonLogLevel daemon_log_level = DAEMON_LOG_NORMAL;

bool daemon_debug_enabled(void)
{
    return false;
}

void daemon_log(const char *fmt, ...)
{
    (void)fmt;
}

void daemon_debug_ctx(const char *ctx, const char *fmt, ...)
{
    (void)ctx;
    (void)fmt;
}

void daemon_debug_band(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}

TxResult lora_send(uint8_t *buf, size_t len, int band)
{
    (void)buf;
    (void)len;
    (void)band;
    return TX_RESULT_RADIO_ERROR;
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

static DaemonTxJob make_job(uint16_t seq)
{
    DaemonTxJob job;
    uint8_t payload[] = { 0x33 };

    daemon_tx_job_init(&job, 433, 0, seq);
    daemon_tx_job_set_payload(&job, payload, sizeof(payload));

    return job;
}

static void test_worker_lookup(void)
{
    daemon_tx_async_runtime_init();

    expect_int("lookup 433 present",
               daemon_tx_async_runtime_worker_for_band(433) != NULL, 1);
    expect_int("lookup 868 present",
               daemon_tx_async_runtime_worker_for_band(868) != NULL, 1);
    expect_int("lookup invalid absent",
               daemon_tx_async_runtime_worker_for_band(123) == NULL, 1);
    expect_size("pending 433 zero",
                daemon_tx_async_runtime_pending_for_band(433), 0);
    expect_size("pending 868 zero",
                daemon_tx_async_runtime_pending_for_band(868), 0);

    daemon_tx_async_runtime_shutdown();
}

static void test_shutdown_resets_queue_state(void)
{
    DaemonTxAsyncWorker *worker;
    DaemonTxJob job = make_job(10);

    daemon_tx_async_runtime_init();
    worker = daemon_tx_async_runtime_worker_for_band(433);

    expect_int("runtime submit before shutdown",
               daemon_tx_async_worker_submit(worker, &job), 0);
    expect_size("runtime pending before shutdown",
                daemon_tx_async_runtime_pending_for_band(433), 1);

    daemon_tx_async_runtime_shutdown();

    expect_size("runtime pending after shutdown",
                daemon_tx_async_runtime_pending_for_band(433), 0);
    expect_size("runtime pending invalid",
                daemon_tx_async_runtime_pending_for_band(123), 0);
}

static void test_init_idempotent(void)
{
    daemon_tx_async_runtime_init();
    daemon_tx_async_runtime_init();

    expect_int("runtime 433 stopped",
               daemon_tx_async_runtime_running_for_band(433), 0);
    expect_int("runtime 868 stopped",
               daemon_tx_async_runtime_running_for_band(868), 0);

    daemon_tx_async_runtime_shutdown();
}

static void test_completion_records_selected_stats(void)
{
    DaemonRadioStats stats_a;
    DaemonRadioStats stats_b;
    DaemonTxJob job = make_job(20);
    DaemonTxJobResult result;

    daemon_radio_stats_init(&stats_a);
    daemon_radio_stats_init(&stats_b);
    daemon_tx_async_runtime_init();

    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_OK);
    daemon_tx_async_runtime_set_stats_for_band(433, &stats_a);
    daemon_tx_async_runtime_record_completion(
        &result,
        daemon_tx_async_runtime_completion_record_ctx_for_band(433));

    expect_size("runtime first stats tx ok", stats_a.tx_ok, 1);
    expect_size("runtime first stats pending",
                daemon_tx_async_runtime_completion_pending_for_band(433),
                1);

    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_BUSY);
    daemon_tx_async_runtime_set_stats_for_band(433, &stats_b);
    daemon_tx_async_runtime_record_completion(
        &result,
        daemon_tx_async_runtime_completion_record_ctx_for_band(433));

    expect_size("runtime old stats unchanged", stats_a.tx_busy, 0);
    expect_size("runtime selected stats busy", stats_b.tx_busy, 1);
    expect_size("runtime second stats pending",
                daemon_tx_async_runtime_completion_pending_for_band(433),
                2);

    daemon_tx_async_runtime_shutdown();
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

    test_worker_lookup();
    test_shutdown_resets_queue_state();
    test_init_idempotent();
    test_completion_records_selected_stats();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
