#ifndef LORAHAM_DAEMON_TX_ASYNC_WORKER_H
#define LORAHAM_DAEMON_TX_ASYNC_WORKER_H

#include <condition_variable>
#include <mutex>
#include <thread>

#include "daemon_tx_worker.h"

/* --- TX-Async-Worker ----------------------------------------------------- */

struct DaemonTxAsyncWorker {
    DaemonTxWorker worker;
    DaemonTxSendFn send_fn;
    void *send_ctx;
    DaemonTxWorkerResultFn result_fn;
    void *result_ctx;
    std::mutex lock;
    std::condition_variable wake;
    std::thread thread;
    bool running;
    bool stop_requested;

    DaemonTxAsyncWorker() :
        send_fn(NULL),
        send_ctx(NULL),
        result_fn(NULL),
        result_ctx(NULL),
        running(false),
        stop_requested(false)
    {
        daemon_tx_worker_init(&worker);
    }
};

static inline void daemon_tx_async_worker_init(DaemonTxAsyncWorker *async)
{
    if (!async)
        return;

    std::lock_guard<std::mutex> guard(async->lock);
    daemon_tx_worker_init(&async->worker);
    async->send_fn = NULL;
    async->send_ctx = NULL;
    async->result_fn = NULL;
    async->result_ctx = NULL;
    async->running = false;
    async->stop_requested = false;
}

static inline void daemon_tx_async_worker_configure(DaemonTxAsyncWorker *async,
                                                    DaemonTxSendFn send_fn,
                                                    void *send_ctx,
                                                    DaemonTxWorkerResultFn result_fn,
                                                    void *result_ctx)
{
    if (!async)
        return;

    std::lock_guard<std::mutex> guard(async->lock);
    async->send_fn = send_fn;
    async->send_ctx = send_ctx;
    async->result_fn = result_fn;
    async->result_ctx = result_ctx;
}

static inline size_t daemon_tx_async_worker_pending(DaemonTxAsyncWorker *async)
{
    if (!async)
        return 0;

    std::lock_guard<std::mutex> guard(async->lock);
    return daemon_tx_worker_pending(&async->worker);
}

static inline size_t daemon_tx_async_worker_accepted(DaemonTxAsyncWorker *async)
{
    if (!async)
        return 0;

    std::lock_guard<std::mutex> guard(async->lock);
    return daemon_tx_worker_accepted(&async->worker);
}

static inline size_t daemon_tx_async_worker_processed(DaemonTxAsyncWorker *async)
{
    if (!async)
        return 0;

    std::lock_guard<std::mutex> guard(async->lock);
    return daemon_tx_worker_processed(&async->worker);
}

static inline size_t daemon_tx_async_worker_rejected(DaemonTxAsyncWorker *async)
{
    if (!async)
        return 0;

    std::lock_guard<std::mutex> guard(async->lock);
    return daemon_tx_worker_rejected(&async->worker);
}

static inline size_t daemon_tx_async_worker_dropped(DaemonTxAsyncWorker *async)
{
    if (!async)
        return 0;

    std::lock_guard<std::mutex> guard(async->lock);
    return daemon_tx_worker_dropped(&async->worker);
}

static inline int daemon_tx_async_worker_running(DaemonTxAsyncWorker *async)
{
    if (!async)
        return 0;

    std::lock_guard<std::mutex> guard(async->lock);
    return async->running ? 1 : 0;
}

static inline const DaemonTxJobResult *daemon_tx_async_worker_last_result_unsafe(DaemonTxAsyncWorker *async)
{
    return async ? daemon_tx_worker_last_result(&async->worker) : NULL;
}

static inline void daemon_tx_async_worker_loop(DaemonTxAsyncWorker *async)
{
    for (;;) {
        DaemonTxJob job;
        DaemonTxSendFn send_fn;
        void *send_ctx;
        DaemonTxWorkerResultFn result_fn;
        void *result_ctx;
        DaemonTxJobResult result;

        {
            std::unique_lock<std::mutex> guard(async->lock);

            while (!async->stop_requested &&
                   daemon_tx_worker_pending(&async->worker) == 0) {
                async->wake.wait(guard);
            }

            if (async->stop_requested &&
                daemon_tx_worker_pending(&async->worker) == 0) {
                async->running = false;
                return;
            }

            if (daemon_tx_queue_pop(&async->worker.queue, &job) != 0)
                continue;

            send_fn = async->send_fn;
            send_ctx = async->send_ctx;
            result_fn = async->result_fn;
            result_ctx = async->result_ctx;
        }

        result = daemon_tx_execute_job_with_sender(&job, send_fn, send_ctx);

        {
            DaemonTxWorkerDrainCtx drain;

            std::lock_guard<std::mutex> guard(async->lock);
            drain.worker = &async->worker;
            drain.result_fn = result_fn;
            drain.result_ctx = result_ctx;
            daemon_tx_worker_record_result(&result, &drain);
        }
    }
}

static inline int daemon_tx_async_worker_start(DaemonTxAsyncWorker *async)
{
    if (!async)
        return -1;

    {
        std::lock_guard<std::mutex> guard(async->lock);

        if (async->running)
            return 0;

        async->stop_requested = false;
        async->running = true;
    }

    try {
        async->thread = std::thread(daemon_tx_async_worker_loop, async);
    } catch (...) {
        std::lock_guard<std::mutex> guard(async->lock);
        async->running = false;
        return -1;
    }

    return 0;
}

static inline void daemon_tx_async_worker_stop(DaemonTxAsyncWorker *async)
{
    if (!async)
        return;

    {
        std::lock_guard<std::mutex> guard(async->lock);
        async->stop_requested = true;
    }

    async->wake.notify_all();

    if (async->thread.joinable())
        async->thread.join();

    {
        std::lock_guard<std::mutex> guard(async->lock);
        async->running = false;
    }
}

static inline int daemon_tx_async_worker_submit(DaemonTxAsyncWorker *async,
                                                const DaemonTxJob *job)
{
    int rc;

    if (!async)
        return -1;

    {
        std::lock_guard<std::mutex> guard(async->lock);
        rc = daemon_tx_worker_submit(&async->worker, job);
    }

    if (rc == 0)
        async->wake.notify_one();

    return rc;
}

#endif
