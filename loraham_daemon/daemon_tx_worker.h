#ifndef LORAHAM_DAEMON_TX_WORKER_H
#define LORAHAM_DAEMON_TX_WORKER_H

#include <stddef.h>
#include <string.h>

#include "daemon_tx_queue.h"

/* --- TX-Worker-Zustand -------------------------------------------------- */

typedef void (*DaemonTxWorkerResultFn)(const DaemonTxJobResult *result,
                                       void *ctx);

typedef struct {
    DaemonTxQueue queue;
    size_t accepted;
    size_t rejected;
    size_t processed;
    int has_last_result;
    DaemonTxJobResult last_result;
} DaemonTxWorker;

typedef struct {
    DaemonTxWorker *worker;
    DaemonTxWorkerResultFn result_fn;
    void *result_ctx;
} DaemonTxWorkerDrainCtx;

static inline void daemon_tx_worker_init(DaemonTxWorker *worker)
{
    if (!worker)
        return;

    memset(worker, 0, sizeof(*worker));
    daemon_tx_queue_init(&worker->queue);
}

static inline size_t daemon_tx_worker_pending(const DaemonTxWorker *worker)
{
    return worker ? daemon_tx_queue_count(&worker->queue) : 0;
}

static inline size_t daemon_tx_worker_accepted(const DaemonTxWorker *worker)
{
    return worker ? worker->accepted : 0;
}

static inline size_t daemon_tx_worker_rejected(const DaemonTxWorker *worker)
{
    return worker ? worker->rejected : 0;
}

static inline size_t daemon_tx_worker_processed(const DaemonTxWorker *worker)
{
    return worker ? worker->processed : 0;
}

static inline size_t daemon_tx_worker_dropped(const DaemonTxWorker *worker)
{
    return worker ? daemon_tx_queue_dropped(&worker->queue) : 0;
}

static inline const DaemonTxJobResult *daemon_tx_worker_last_result(const DaemonTxWorker *worker)
{
    if (!worker || !worker->has_last_result)
        return NULL;

    return &worker->last_result;
}

static inline int daemon_tx_worker_submit(DaemonTxWorker *worker,
                                          const DaemonTxJob *job)
{
    int rc;

    if (!worker)
        return -1;

    rc = daemon_tx_queue_push(&worker->queue, job);
    if (rc == 0)
        worker->accepted++;
    else
        worker->rejected++;

    return rc;
}

static inline void daemon_tx_worker_record_result(const DaemonTxJobResult *result,
                                                  void *ctx)
{
    DaemonTxWorkerDrainCtx *drain = (DaemonTxWorkerDrainCtx *)ctx;

    if (!drain || !drain->worker || !result)
        return;

    drain->worker->last_result = *result;
    drain->worker->has_last_result = 1;
    drain->worker->processed++;

    if (drain->result_fn)
        drain->result_fn(result, drain->result_ctx);
}

static inline size_t daemon_tx_worker_drain(DaemonTxWorker *worker,
                                            DaemonTxSendFn send_fn,
                                            void *send_ctx,
                                            DaemonTxWorkerResultFn result_fn,
                                            void *result_ctx,
                                            size_t max_jobs)
{
    DaemonTxWorkerDrainCtx drain;

    if (!worker)
        return 0;

    drain.worker = worker;
    drain.result_fn = result_fn;
    drain.result_ctx = result_ctx;

    return daemon_tx_queue_drain(&worker->queue,
                                 send_fn,
                                 send_ctx,
                                 daemon_tx_worker_record_result,
                                 &drain,
                                 max_jobs);
}

#endif
