#ifndef LORAHAM_DAEMON_TX_COMPLETION_H
#define LORAHAM_DAEMON_TX_COMPLETION_H

#include <stddef.h>
#include <stdint.h>
#include <mutex>

#include "client_slot.h"
#include "daemon_tx_job.h"
#include "framed_data.h"

/* --- TX-Abschlussmeldung ------------------------------------------------- */

#define DAEMON_TX_COMPLETION_QUEUE_CAPACITY 16

static inline size_t daemon_tx_completion_frame_len(void)
{
    return framed_data_frame_size(FRAMED_DATA_TX_RESULT_PAYLOAD_LEN);
}

static inline int daemon_tx_completion_encode_frame(uint8_t *frame,
                                                    size_t frame_len,
                                                    const DaemonTxJobResult *result)
{
    if (!result)
        return -1;

    return framed_data_encode_tx_result(frame,
                                        frame_len,
                                        result->framed_status,
                                        result->flags,
                                        result->seq);
}



static inline int daemon_tx_completion_deliver_to_slot(ClientSlot *slots,
                                                       int max_slots,
                                                       const DaemonTxJobResult *result)
{
    uint8_t frame[FRAMED_DATA_HEADER_LEN + FRAMED_DATA_TX_RESULT_PAYLOAD_LEN];
    int idx;

    if (!slots || max_slots <= 0 || !result)
        return -1;

    idx = result->completion_slot;
    if (idx == DAEMON_TX_COMPLETION_SLOT_NONE)
        return 0;

    if (idx < 0 || idx >= max_slots)
        return 0;

    if (!client_slot_has_client(&slots[idx]))
        return 0;

    if (result->completion_generation != 0u &&
        client_slot_generation(&slots[idx]) != result->completion_generation)
        return 0;

    if (daemon_tx_completion_encode_frame(frame, sizeof(frame), result) != 0)
        return -1;

    if (!client_output_queue_append(&slots[idx].output, frame, sizeof(frame))) {
        client_slot_close(&slots[idx]);
        return -1;
    }

    client_slot_flush_output(&slots[idx]);
    return 1;
}


/* --- TX-Abschluss-Warteschlange ------------------------------------------ */

struct DaemonTxCompletionQueue {
    std::mutex lock;
    DaemonTxJobResult entries[DAEMON_TX_COMPLETION_QUEUE_CAPACITY];
    size_t head;
    size_t count;
    size_t dropped;

    DaemonTxCompletionQueue() : head(0), count(0), dropped(0) {}
};

static inline void daemon_tx_completion_queue_init(DaemonTxCompletionQueue *queue)
{
    if (!queue)
        return;

    std::lock_guard<std::mutex> guard(queue->lock);
    queue->head = 0;
    queue->count = 0;
    queue->dropped = 0;
}

static inline size_t daemon_tx_completion_queue_pending(DaemonTxCompletionQueue *queue)
{
    if (!queue)
        return 0;

    std::lock_guard<std::mutex> guard(queue->lock);
    return queue->count;
}

static inline size_t daemon_tx_completion_queue_dropped(DaemonTxCompletionQueue *queue)
{
    if (!queue)
        return 0;

    std::lock_guard<std::mutex> guard(queue->lock);
    return queue->dropped;
}

static inline int daemon_tx_completion_queue_push(DaemonTxCompletionQueue *queue,
                                                  const DaemonTxJobResult *result)
{
    size_t idx;

    if (!queue || !result)
        return -1;

    std::lock_guard<std::mutex> guard(queue->lock);

    if (queue->count >= DAEMON_TX_COMPLETION_QUEUE_CAPACITY) {
        queue->head = (queue->head + 1) % DAEMON_TX_COMPLETION_QUEUE_CAPACITY;
        queue->count--;
        queue->dropped++;
    }

    idx = (queue->head + queue->count) % DAEMON_TX_COMPLETION_QUEUE_CAPACITY;
    queue->entries[idx] = *result;
    queue->count++;

    return 0;
}

static inline int daemon_tx_completion_queue_pop(DaemonTxCompletionQueue *queue,
                                                 DaemonTxJobResult *out)
{
    if (!queue || !out)
        return -1;

    std::lock_guard<std::mutex> guard(queue->lock);

    if (queue->count == 0)
        return -1;

    *out = queue->entries[queue->head];
    queue->head = (queue->head + 1) % DAEMON_TX_COMPLETION_QUEUE_CAPACITY;
    queue->count--;

    return 0;
}


#endif
