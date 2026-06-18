#include "daemon_tx_async_runtime.h"

#include "daemon_log.h"

/* --- TX-Async-Runtime ---------------------------------------------------- */

DaemonTxAsyncWorker daemon_tx_async_worker_433;
DaemonTxAsyncWorker daemon_tx_async_worker_868;

void daemon_tx_async_runtime_init(void)
{
    daemon_debug_ctx("TXASYNC", "Worker initialisieren");

    daemon_tx_async_worker_init(&daemon_tx_async_worker_433);
    daemon_tx_async_worker_init(&daemon_tx_async_worker_868);
}

void daemon_tx_async_runtime_shutdown(void)
{
    daemon_debug_ctx("TXASYNC", "Worker stoppen");

    daemon_tx_async_worker_stop(&daemon_tx_async_worker_433);
    daemon_tx_async_worker_stop(&daemon_tx_async_worker_868);

    daemon_tx_async_worker_init(&daemon_tx_async_worker_433);
    daemon_tx_async_worker_init(&daemon_tx_async_worker_868);
}

DaemonTxAsyncWorker *daemon_tx_async_runtime_worker_for_band(int band)
{
    if (band == 433)
        return &daemon_tx_async_worker_433;

    if (band == 868)
        return &daemon_tx_async_worker_868;

    return NULL;
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

int daemon_tx_async_runtime_running_for_band(int band)
{
    return daemon_tx_async_worker_running(daemon_tx_async_runtime_worker_for_band(band));
}
