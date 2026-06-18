#ifndef LORAHAM_DAEMON_TX_ASYNC_RUNTIME_H
#define LORAHAM_DAEMON_TX_ASYNC_RUNTIME_H

#include <stddef.h>

#include "daemon_tx_async_worker.h"
#include "daemon_tx_completion.h"

/* --- TX-Async-Runtime ---------------------------------------------------- */

extern DaemonTxAsyncWorker daemon_tx_async_worker_433;
extern DaemonTxAsyncWorker daemon_tx_async_worker_868;
extern DaemonTxCompletionQueue daemon_tx_async_completion_queue_868;
extern DaemonTxCompletionQueue daemon_tx_async_completion_queue_433;

void daemon_tx_async_runtime_init(void);
void daemon_tx_async_runtime_shutdown(void);
DaemonTxAsyncWorker *daemon_tx_async_runtime_worker_for_band(int band);
int daemon_tx_async_runtime_pop_completion_for_band(int band, DaemonTxJobResult *out);
size_t daemon_tx_async_runtime_completion_dropped_for_band(int band);
size_t daemon_tx_async_runtime_completion_stale_for_band(int band);
void daemon_tx_async_runtime_record_completion_stale_for_band(int band);
size_t daemon_tx_async_runtime_completion_pending_for_band(int band);
void daemon_tx_async_runtime_record_completion(const DaemonTxJobResult *result, void *ctx);
DaemonTxCompletionQueue *daemon_tx_async_runtime_completion_queue_for_band(int band);
size_t daemon_tx_async_runtime_pending_for_band(int band);
size_t daemon_tx_async_runtime_dropped_for_band(int band);
size_t daemon_tx_async_runtime_processed_for_band(int band);
size_t daemon_tx_async_runtime_rejected_for_band(int band);
size_t daemon_tx_async_runtime_accepted_for_band(int band);
int daemon_tx_async_runtime_last_result_for_band(int band, DaemonTxJobResult *out);
int daemon_tx_async_runtime_running_for_band(int band);

#endif
