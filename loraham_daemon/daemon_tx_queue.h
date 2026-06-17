#ifndef LORAHAM_DAEMON_TX_QUEUE_H
#define LORAHAM_DAEMON_TX_QUEUE_H

#include <stddef.h>
#include <string.h>

#include "daemon_tx_executor.h"
#include "daemon_tx_job.h"

/* --- Interne TX-Warteschlange ------------------------------------------ */

#ifndef DAEMON_TX_QUEUE_CAPACITY
#define DAEMON_TX_QUEUE_CAPACITY 8
#endif

typedef void (*DaemonTxQueueResultFn)(const DaemonTxJobResult *result,
                                      void *ctx);

typedef struct {
    DaemonTxJob jobs[DAEMON_TX_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    size_t dropped;
} DaemonTxQueue;

static inline void daemon_tx_queue_init(DaemonTxQueue *queue)
{
    if (!queue)
        return;

    memset(queue, 0, sizeof(*queue));
}

static inline size_t daemon_tx_queue_count(const DaemonTxQueue *queue)
{
    return queue ? queue->count : 0;
}

static inline size_t daemon_tx_queue_dropped(const DaemonTxQueue *queue)
{
    return queue ? queue->dropped : 0;
}

static inline int daemon_tx_queue_empty(const DaemonTxQueue *queue)
{
    return !queue || queue->count == 0;
}

static inline int daemon_tx_queue_full(const DaemonTxQueue *queue)
{
    return queue && queue->count >= DAEMON_TX_QUEUE_CAPACITY;
}

static inline int daemon_tx_queue_push(DaemonTxQueue *queue,
                                       const DaemonTxJob *job)
{
    if (!queue || !job)
        return -1;

    if (daemon_tx_queue_full(queue)) {
        queue->dropped++;
        return -1;
    }

    queue->jobs[queue->tail] = *job;
    queue->tail = (queue->tail + 1) % DAEMON_TX_QUEUE_CAPACITY;
    queue->count++;

    return 0;
}

static inline int daemon_tx_queue_pop(DaemonTxQueue *queue,
                                      DaemonTxJob *job)
{
    if (!queue || !job || queue->count == 0)
        return -1;

    *job = queue->jobs[queue->head];
    queue->head = (queue->head + 1) % DAEMON_TX_QUEUE_CAPACITY;
    queue->count--;

    return 0;
}

static inline size_t daemon_tx_queue_drain(DaemonTxQueue *queue,
                                           DaemonTxSendFn send_fn,
                                           void *send_ctx,
                                           DaemonTxQueueResultFn result_fn,
                                           void *result_ctx,
                                           size_t max_jobs)
{
    size_t processed = 0;
    DaemonTxJob job;

    if (!queue)
        return 0;

    while (queue->count > 0 && (max_jobs == 0 || processed < max_jobs)) {
        DaemonTxJobResult result;

        if (daemon_tx_queue_pop(queue, &job) != 0)
            break;

        result = daemon_tx_execute_job_with_sender(&job, send_fn, send_ctx);

        if (result_fn)
            result_fn(&result, result_ctx);

        processed++;
    }

    return processed;
}

#endif
