#ifndef LORAHAM_DAEMON_TX_ASYNC_RUNTIME_H
#define LORAHAM_DAEMON_TX_ASYNC_RUNTIME_H

#include <stddef.h>

#include "daemon_tx_async_worker.h"

/* --- TX-Async-Runtime ---------------------------------------------------- */

extern DaemonTxAsyncWorker daemon_tx_async_worker_433;
extern DaemonTxAsyncWorker daemon_tx_async_worker_868;

void daemon_tx_async_runtime_init(void);
void daemon_tx_async_runtime_shutdown(void);
DaemonTxAsyncWorker *daemon_tx_async_runtime_worker_for_band(int band);
size_t daemon_tx_async_runtime_pending_for_band(int band);
size_t daemon_tx_async_runtime_dropped_for_band(int band);
size_t daemon_tx_async_runtime_processed_for_band(int band);
size_t daemon_tx_async_runtime_rejected_for_band(int band);
size_t daemon_tx_async_runtime_accepted_for_band(int band);
int daemon_tx_async_runtime_running_for_band(int band);

#endif
