#include "daemon_tx_async_runtime.h"

#include "daemon_log.h"

/* --- TX-Async-Runtime ---------------------------------------------------- */

typedef struct {
    DaemonTxCompletionQueue *completion_queue;
    DaemonRadioStats *stats;
} DaemonTxAsyncCompletionRecordCtx;

DaemonTxAsyncWorker daemon_tx_async_worker_433;
DaemonTxAsyncWorker daemon_tx_async_worker_868;
DaemonTxCompletionQueue daemon_tx_async_completion_queue_433;
DaemonTxCompletionQueue daemon_tx_async_completion_queue_868;
static size_t daemon_tx_async_completion_stale_433;
static size_t daemon_tx_async_completion_stale_868;
static DaemonTxAsyncCompletionRecordCtx daemon_tx_async_completion_record_433;
static DaemonTxAsyncCompletionRecordCtx daemon_tx_async_completion_record_868;

void daemon_tx_async_runtime_init(void)
{
    daemon_debug_ctx("TXASYNC", "Worker initialisieren");

    daemon_tx_async_worker_init(&daemon_tx_async_worker_433);
    daemon_tx_async_worker_init(&daemon_tx_async_worker_868);
    daemon_tx_completion_queue_init(&daemon_tx_async_completion_queue_433);
    daemon_tx_completion_queue_init(&daemon_tx_async_completion_queue_868);
    daemon_tx_async_completion_record_433.completion_queue =
        &daemon_tx_async_completion_queue_433;
    daemon_tx_async_completion_record_433.stats = NULL;
    daemon_tx_async_completion_record_868.completion_queue =
        &daemon_tx_async_completion_queue_868;
    daemon_tx_async_completion_record_868.stats = NULL;
    daemon_tx_async_completion_stale_433 = 0;
    daemon_tx_async_completion_stale_868 = 0;
}

void daemon_tx_async_runtime_shutdown(void)
{
    daemon_debug_ctx("TXASYNC", "Worker stoppen");

    daemon_tx_async_worker_stop(&daemon_tx_async_worker_433);
    daemon_tx_async_worker_stop(&daemon_tx_async_worker_868);

    daemon_tx_async_worker_init(&daemon_tx_async_worker_433);
    daemon_tx_async_worker_init(&daemon_tx_async_worker_868);
    daemon_tx_async_completion_record_433.stats = NULL;
    daemon_tx_async_completion_record_868.stats = NULL;
}

DaemonTxAsyncWorker *daemon_tx_async_runtime_worker_for_band(int band)
{
    if (band == 433)
        return &daemon_tx_async_worker_433;

    if (band == 868)
        return &daemon_tx_async_worker_868;

    return NULL;
}


DaemonTxCompletionQueue *daemon_tx_async_runtime_completion_queue_for_band(int band)
{
    if (band == 433)
        return &daemon_tx_async_completion_queue_433;

    if (band == 868)
        return &daemon_tx_async_completion_queue_868;

    return NULL;
}

static DaemonTxAsyncCompletionRecordCtx *daemon_tx_async_runtime_record_ctx_for_band(int band)
{
    if (band == 433)
        return &daemon_tx_async_completion_record_433;

    if (band == 868)
        return &daemon_tx_async_completion_record_868;

    return NULL;
}

void daemon_tx_async_runtime_set_stats_for_band(int band, DaemonRadioStats *stats)
{
    DaemonTxAsyncCompletionRecordCtx *record =
        daemon_tx_async_runtime_record_ctx_for_band(band);

    if (record)
        record->stats = stats;
}

void *daemon_tx_async_runtime_completion_record_ctx_for_band(int band)
{
    return daemon_tx_async_runtime_record_ctx_for_band(band);
}

static size_t *daemon_tx_async_runtime_stale_counter_for_band(int band)
{
    if (band == 433)
        return &daemon_tx_async_completion_stale_433;

    if (band == 868)
        return &daemon_tx_async_completion_stale_868;

    return NULL;
}

void daemon_tx_async_runtime_record_completion_stale_for_band(int band)
{
    size_t *counter = daemon_tx_async_runtime_stale_counter_for_band(band);

    if (counter)
        (*counter)++;
}

size_t daemon_tx_async_runtime_completion_stale_for_band(int band)
{
    size_t *counter = daemon_tx_async_runtime_stale_counter_for_band(band);

    return counter ? *counter : 0;
}

void daemon_tx_async_runtime_record_completion(const DaemonTxJobResult *result,
                                               void *ctx)
{
    DaemonTxAsyncCompletionRecordCtx *record =
        (DaemonTxAsyncCompletionRecordCtx *)ctx;

    if (!record || !result)
        return;

    if (record->stats) {
        daemon_radio_stats_record_tx_result(record->stats, result->tx_result);
        if ((result->flags & FRAMED_DATA_TX_RESULT_FLAG_CAD_TIMEOUT) &&
            result->tx_result == TX_RESULT_OK) {
            daemon_radio_stats_record_cad_timeout_send(record->stats);
        }
    }

    daemon_tx_completion_queue_push(record->completion_queue, result);
}

size_t daemon_tx_async_runtime_completion_pending_for_band(int band)
{
    return daemon_tx_completion_queue_pending(
        daemon_tx_async_runtime_completion_queue_for_band(band));
}

size_t daemon_tx_async_runtime_completion_dropped_for_band(int band)
{
    return daemon_tx_completion_queue_dropped(
        daemon_tx_async_runtime_completion_queue_for_band(band));
}

int daemon_tx_async_runtime_pop_completion_for_band(int band,
                                                    DaemonTxJobResult *out)
{
    return daemon_tx_completion_queue_pop(
        daemon_tx_async_runtime_completion_queue_for_band(band), out);
}

size_t daemon_tx_async_runtime_pending_for_band(int band)
{
    return daemon_tx_async_worker_pending(daemon_tx_async_runtime_worker_for_band(band));
}


size_t daemon_tx_async_runtime_accepted_for_band(int band)
{
    return daemon_tx_async_worker_accepted(daemon_tx_async_runtime_worker_for_band(band));
}

size_t daemon_tx_async_runtime_rejected_for_band(int band)
{
    return daemon_tx_async_worker_rejected(daemon_tx_async_runtime_worker_for_band(band));
}

size_t daemon_tx_async_runtime_processed_for_band(int band)
{
    return daemon_tx_async_worker_processed(daemon_tx_async_runtime_worker_for_band(band));
}

size_t daemon_tx_async_runtime_dropped_for_band(int band)
{
    return daemon_tx_async_worker_dropped(daemon_tx_async_runtime_worker_for_band(band));
}

int daemon_tx_async_runtime_last_result_for_band(int band, DaemonTxJobResult *out)
{
    return daemon_tx_async_worker_last_result_copy(daemon_tx_async_runtime_worker_for_band(band),
                                                  out);
}

int daemon_tx_async_runtime_running_for_band(int band)
{
    return daemon_tx_async_worker_running(daemon_tx_async_runtime_worker_for_band(band));
}
