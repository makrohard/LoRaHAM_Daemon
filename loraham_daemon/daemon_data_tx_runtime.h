#ifndef LORAHAM_DAEMON_DATA_TX_RUNTIME_H
#define LORAHAM_DAEMON_DATA_TX_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "data_tx.h"
#include "daemon_log.h"
#include "daemon_radio_runtime.h"
#include "daemon_stats.h"
#include "daemon_tx.h"
#include "daemon_tx_async_runtime.h"
#include "daemon_tx_executor.h"
#include "daemon_tx_outcome.h"
#include "daemon_tx_policy.h"
#include "radio_cad.h"
#include "radio_controller.h"
#include "tx_result.h"

/* --- DATA TX runtime helpers -------------------------------------------- */
#define DATA_TX_CAD_WAIT_FREE 0
#define DATA_TX_CAD_WAIT_BLOCK 1
#define DATA_TX_CAD_WAIT_TIMEOUT_SEND 2

static inline int data_tx_cad_wait_blocks_tx(int decision)
{
    return decision == DATA_TX_CAD_WAIT_BLOCK;
}

static inline int data_tx_cad_wait_timed_out(int decision)
{
    return decision == DATA_TX_CAD_WAIT_TIMEOUT_SEND;
}

static inline void data_tx_apply_cad_decision_flags(DaemonTxJob *job,
                                                    int decision)
{
    if (!job)
        return;

    if (data_tx_cad_wait_timed_out(decision))
        job->flags |= FRAMED_DATA_TX_RESULT_FLAG_CAD_TIMEOUT;
}

template<typename RadioT>
struct DataTxDaemonContext {
    RadioController<RadioT> *ctrl;
    const char *log_ctx;
    DaemonTxSendFn send_fn;
    void *send_ctx;
    int completion_slot;
    uint16_t completion_seq;
    uint32_t completion_generation;
    uint32_t tx_busy_wait_ticks;
    useconds_t tx_busy_sleep_usec;
    uint32_t cad_wait_ticks;
    uint32_t cad_idle_stable_ticks;
    useconds_t cad_sleep_usec;
};

void daemon_data_tx_trace_message(void *ctx, const char *msg);
DataTxLog daemon_data_tx_log(const char *ctx);


template<typename RadioT>
static int daemon_data_tx_result_deferred(void *ctx)
{
    DataTxDaemonContext<RadioT> *tx =
        (DataTxDaemonContext<RadioT> *)ctx;

    return tx && tx->ctrl && tx->ctrl->tx_queue_active.load();
}

template<typename RadioT>
static void daemon_data_tx_set_completion_target(void *ctx,
                                                 int slot_index,
                                                 uint32_t generation,
                                                 uint16_t seq)
{
    DataTxDaemonContext<RadioT> *tx =
        (DataTxDaemonContext<RadioT> *)ctx;

    if (!tx)
        return;

    tx->completion_slot = slot_index;
    tx->completion_generation = generation;
    tx->completion_seq = seq;
}

struct DataTxCadPolicy {
    uint32_t   cad_wait_ticks;
    uint32_t   cad_idle_stable_ticks;
    useconds_t cad_sleep_usec;
    bool       send_after_timeout;
};

template<typename RadioT>
static DataTxCadPolicy data_tx_snapshot_cad_policy(
    const RadioController<RadioT> *ctrl)
{
    DataTxCadPolicy p;

    if (!ctrl) {
        p.cad_wait_ticks        = daemon_tx_policy_cad_wait_ticks();
        p.cad_idle_stable_ticks = daemon_tx_policy_cad_idle_stable_ticks();
        p.cad_sleep_usec        = (useconds_t)daemon_tx_policy_poll_interval_usec();
        p.send_after_timeout    = DAEMON_TX_POLICY_SEND_AFTER_CAD_TIMEOUT ? true : false;
        return p;
    }

    uint32_t wait_ms = ctrl->cad_wait_timeout_ms.load();
    uint32_t idle_ms = ctrl->cad_idle_stable_ms.load();
    uint32_t poll_ms = ctrl->cad_poll_interval_ms.load();

    p.cad_wait_ticks        = daemon_tx_policy_ticks_for_ms(wait_ms, poll_ms);
    p.cad_idle_stable_ticks = daemon_tx_policy_ticks_for_ms(idle_ms, poll_ms);
    p.cad_sleep_usec        = (useconds_t)(poll_ms * 1000u);
    p.send_after_timeout    = ctrl->cad_send_after_timeout.load();
    return p;
}

template<typename RadioT>
static int data_tx_probe_channel_busy(DataTxDaemonContext<RadioT> *tx)
{
    RadioController<RadioT> *ctrl = tx ? tx->ctrl : NULL;
    RadioCadProbeResult probe;

    if (!ctrl || ctrl->mode != RADIO_MODE_LORA)
        return 0;

    probe = radio_cad_probe(ctrl);
    return probe.status == RADIO_CAD_PROBE_BUSY;
}

template<typename RadioT>
static int data_tx_wait_channel_free_with_limits_ex(
    DataTxDaemonContext<RadioT> *tx,
    uint32_t max_ticks,
    uint32_t stable_idle_ticks,
    useconds_t sleep_usec,
    bool send_after_timeout)
{
    RadioController<RadioT> *ctrl = tx ? tx->ctrl : NULL;
    uint32_t cad_wait = 0;
    uint32_t free_ticks = 0;
    uint32_t required_free_ticks = stable_idle_ticks ? stable_idle_ticks : 1u;

    if (!ctrl || ctrl->mode != RADIO_MODE_LORA)
        return DATA_TX_CAD_WAIT_FREE;

    if (ctrl->tx_mode == RADIO_TX_MODE_DIRECT) {
        // DIRECT: transmit immediately, no CAD probe, no wait, no block.
        return DATA_TX_CAD_WAIT_FREE;
    }

    while (cad_wait < max_ticks) {
        if (!data_tx_probe_channel_busy(tx)) {
            free_ticks++;
            if (free_ticks >= required_free_ticks)
                return DATA_TX_CAD_WAIT_FREE;
        } else {
            free_ticks = 0;
        }

        cad_wait++;
        if (sleep_usec > 0 && cad_wait < max_ticks)
            usleep(sleep_usec);
    }

    return send_after_timeout ?
           DATA_TX_CAD_WAIT_TIMEOUT_SEND :
           DATA_TX_CAD_WAIT_BLOCK;
}

template<typename RadioT>
static int data_tx_wait_channel_free_with_limits(DataTxDaemonContext<RadioT> *tx,
                                                 uint32_t max_ticks,
                                                 uint32_t stable_idle_ticks,
                                                 useconds_t sleep_usec)
{
    return data_tx_wait_channel_free_with_limits_ex(
        tx, max_ticks, stable_idle_ticks, sleep_usec, false);
}

template<typename RadioT>
static int data_tx_wait_channel_free(DataTxDaemonContext<RadioT> *tx)
{
    RadioController<RadioT> *ctrl = tx ? tx->ctrl : NULL;
    DataTxCadPolicy p = data_tx_snapshot_cad_policy(ctrl);
    return data_tx_wait_channel_free_with_limits_ex(
        tx, p.cad_wait_ticks, p.cad_idle_stable_ticks,
        p.cad_sleep_usec, p.send_after_timeout);
}

template<typename RadioT>
static int data_tx_wait_tx_ready_with_limits(RadioController<RadioT> *ctrl,
                                             uint32_t max_ticks,
                                             useconds_t sleep_usec)
{
    uint32_t tx_wait = 0;

    if (!ctrl)
        return 1;

    while (ctrl->tx_busy.load()) {
        if (tx_wait >= max_ticks)
            return 1;

        tx_wait++;
        if (sleep_usec > 0 && tx_wait < max_ticks)
            usleep(sleep_usec);
    }

    return 0;
}

template<typename RadioT>
static int data_tx_wait_tx_ready(RadioController<RadioT> *ctrl)
{
    return data_tx_wait_tx_ready_with_limits(
        ctrl,
        daemon_tx_policy_busy_timeout_ticks(),
        (useconds_t)daemon_tx_policy_poll_interval_usec());
}

template<typename RadioT>
static int daemon_data_tx_result_enabled(void *ctx)
{
    DataTxDaemonContext<RadioT> *tx =
        (DataTxDaemonContext<RadioT> *)ctx;

    return tx && tx->ctrl && tx->ctrl->tx_result_active.load();
}

// Managed flag for an immediate framed TX_RESULT: set only in MANAGED mode
// (DIRECT sends without CAD/LBT and must not advertise the managed flag).
template<typename RadioT>
static uint8_t daemon_data_tx_managed_flag(void *ctx)
{
    DataTxDaemonContext<RadioT> *tx =
        (DataTxDaemonContext<RadioT> *)ctx;

    if (tx && tx->ctrl && tx->ctrl->tx_mode == RADIO_TX_MODE_MANAGED)
        return FRAMED_DATA_TX_RESULT_FLAG_MANAGED;

    return 0;
}

template<typename RadioT>
static uint16_t daemon_data_tx_next_result_seq(void *ctx)
{
    DataTxDaemonContext<RadioT> *tx =
        (DataTxDaemonContext<RadioT> *)ctx;

    if (!tx || !tx->ctrl)
        return 0;

    tx->ctrl->tx_result_seq++;
    return tx->ctrl->tx_result_seq;
}


template<typename RadioT>
static int daemon_data_tx_worker_cad_probe(int band, void *ctx)
{
    RadioController<RadioT> *ctrl = (RadioController<RadioT> *)ctx;
    RadioCadProbeResult probe;

    if (!ctrl || band != radio_controller_band_number(ctrl))
        return DAEMON_TX_CAD_PROBE_UNAVAILABLE;

    std::lock_guard<std::recursive_mutex> radio_lock(ctrl->radio_mutex);

    if (!radio_controller_ready(ctrl))
        return DAEMON_TX_CAD_PROBE_UNAVAILABLE;

    if (ctrl->mode != RADIO_MODE_LORA)
        return DAEMON_TX_CAD_PROBE_FREE;

    probe = radio_cad_probe(ctrl);
    if (probe.status == RADIO_CAD_PROBE_FREE)
        return DAEMON_TX_CAD_PROBE_FREE;

    if (probe.status == RADIO_CAD_PROBE_BUSY)
        return DAEMON_TX_CAD_PROBE_BUSY;

    return DAEMON_TX_CAD_PROBE_UNAVAILABLE;
}

static inline void daemon_data_tx_worker_cad_sleep(uint32_t usec, void *ctx)
{
    (void)ctx;

    if (usec > 0)
        usleep((useconds_t)usec);
}

template<typename RadioT>
static void daemon_data_tx_configure_worker_cad(DaemonTxAsyncWorker *async,
                                                DataTxDaemonContext<RadioT> *tx,
                                                const DaemonTxJob *job)
{
    if (!async)
        return;

    (void)job;

    if (!tx || !tx->ctrl) {
        daemon_tx_async_worker_configure_cad(async, NULL, NULL, NULL, NULL);
        return;
    }

    daemon_tx_async_worker_configure_cad(async,
                                         daemon_data_tx_worker_cad_probe<RadioT>,
                                         tx->ctrl,
                                         daemon_data_tx_worker_cad_sleep,
                                         NULL);
}

template<typename RadioT>
static void data_tx_configure_job_cad_policy(DataTxDaemonContext<RadioT> *tx,
                                             DaemonTxJob *job)
{
    RadioController<RadioT> *ctrl = tx ? tx->ctrl : NULL;

    if (!tx || !job || !ctrl || ctrl->mode != RADIO_MODE_LORA) {
        daemon_tx_job_configure_cad_policy(job, 0, 0, 0, 0, 0);
        return;
    }

    if (ctrl->tx_mode == RADIO_TX_MODE_DIRECT) {
        // DIRECT: disable CAD entirely so the worker transmits immediately.
        daemon_tx_job_configure_cad_policy(job, 0, 0, 0, 0, 0);
        return;
    }

    DataTxCadPolicy cad_p = data_tx_snapshot_cad_policy(ctrl);
    daemon_tx_job_configure_cad_policy(job,
                                       1,
                                       cad_p.cad_wait_ticks,
                                       cad_p.cad_idle_stable_ticks,
                                       (uint32_t)cad_p.cad_sleep_usec,
                                       cad_p.send_after_timeout ? 1 : 0);
}

template<typename RadioT>
static DaemonTxJobResult daemon_data_tx_execute_job(DataTxDaemonContext<RadioT> *tx,
                                                    const DaemonTxJob *job,
                                                    DaemonTxSendFn send_fn)
{
    DaemonTxJobResult result;
    DaemonTxAsyncWorker *async;
    DaemonTxCompletionQueue *completion_queue;
    void *completion_record_ctx;

    daemon_tx_job_result_init(&result,
                              job,
                              DAEMON_TX_OUTCOME_RADIO_ERROR);

    if (!tx || !tx->ctrl || !job)
        return result;

    if (!tx->ctrl->tx_queue_active.load())
        return daemon_tx_execute_job_with_sender(job, send_fn, tx->send_ctx);

    async = daemon_tx_async_runtime_worker_for_band(job->band);
    completion_queue = daemon_tx_async_runtime_completion_queue_for_band(job->band);
    completion_record_ctx =
        daemon_tx_async_runtime_completion_record_ctx_for_band(job->band);
    if (!async || !completion_queue || !completion_record_ctx)
        return result;

    daemon_tx_async_runtime_set_stats_for_band(job->band, &tx->ctrl->stats);

    daemon_tx_async_worker_configure(async,
                                     send_fn,
                                     tx->send_ctx,
                                     daemon_tx_async_runtime_record_completion,
                                     completion_record_ctx);
    daemon_data_tx_configure_worker_cad(async, tx, job);

    if (daemon_tx_async_worker_start(async) != 0)
        return result;

    if (daemon_tx_async_worker_submit(async, job) != 0) {
        daemon_tx_job_result_init(&result,
                                  job,
                                  DAEMON_TX_OUTCOME_CHANNEL_BUSY);
        result.tx_result = TX_RESULT_BUSY;
        return result;
    }

    daemon_tx_job_result_init(&result,
                              job,
                              DAEMON_TX_OUTCOME_OK);
    result.tx_result = TX_RESULT_OK;
    return result;
}

template<typename RadioT>
static int send_data_chunk(uint8_t *chunk, size_t len, size_t offset, void *ctx)
{
    DataTxDaemonContext<RadioT> *tx = (DataTxDaemonContext<RadioT> *)ctx;
    RadioController<RadioT> *ctrl = tx->ctrl;
    const char *tag = radio_controller_tag(ctrl);
    int band = radio_controller_band_number(ctrl);

    if (!radio_controller_ready(ctrl)) {
        daemon_debug_ctx(tx->log_ctx, "Radio nicht bereit");
        printf("[%s] DATA-TX abgebrochen: RADIO_NOT_READY\n", tag);
        return DAEMON_TX_OUTCOME_RADIO_NOT_READY;
    }

    if (!ctrl->tx_queue_active.load() &&
        data_tx_wait_tx_ready_with_limits(ctrl,
                                          tx->tx_busy_wait_ticks,
                                          tx->tx_busy_sleep_usec)) {
        daemon_radio_stats_record_tx_result(&ctrl->stats, TX_RESULT_BUSY);
        daemon_debug_ctx(tx->log_ctx, "TX belegt");
        printf("[%s] DATA-TX abgebrochen: %s\n", tag,
               tx_result_name(TX_RESULT_BUSY));
        return DAEMON_TX_OUTCOME_CHANNEL_BUSY;
    }

    int queued_tx = ctrl->tx_queue_active.load();
    int cad_decision = DATA_TX_CAD_WAIT_FREE;

    // CAD guard: LoRa only. Queued TX runs CAD in the worker.
    if (ctrl->mode == RADIO_MODE_LORA) {
        if (queued_tx)
            daemon_debug_ctx(tx->log_ctx, "CAD wird im Worker geprüft");
        else
            daemon_debug_ctx(tx->log_ctx, "CAD prüfen");
    }

    if (!queued_tx) {
        cad_decision = data_tx_wait_channel_free(tx);

        if (data_tx_cad_wait_blocks_tx(cad_decision)) {
            // Only MANAGED can block now; DIRECT never reaches this path.
            TxResult busy_result = TX_RESULT_CAD_TIMEOUT;
            daemon_radio_stats_record_tx_result(&ctrl->stats, busy_result);
            daemon_debug_ctx(tx->log_ctx, "Kanal belegt");
            printf("[%s] Kanal belegt, Paket verworfen\n", tag);
            printf("[%s] DATA-TX abgebrochen: %s\n", tag,
                   tx_result_name(busy_result));
            return DAEMON_TX_OUTCOME_CHANNEL_BUSY;
        }

        if (data_tx_cad_wait_timed_out(cad_decision)) {
            daemon_radio_stats_record_cad_timeout_send(&ctrl->stats);
            daemon_debug_ctx(tx->log_ctx, "CAD Timeout, sende trotzdem");
        }
    }

    daemon_debug_ctx(tx->log_ctx, "Chunk %zu Byte Offset %zu", len, offset);

    DaemonTxJob job;
    DaemonTxJobResult result;
    DaemonTxSendFn send_fn = tx->send_fn ?
                             tx->send_fn :
                             daemon_tx_executor_default_send;

    daemon_tx_job_init(&job, band, ctrl->tx_mode, tx->completion_seq);
    job.completion_slot = tx->completion_slot;
    job.completion_generation = tx->completion_generation;
    // Only MANAGED performs CAD/LBT; DIRECT sends immediately, so its result
    // must not advertise the managed flag.
    job.flags = (ctrl->tx_mode == RADIO_TX_MODE_MANAGED)
                    ? FRAMED_DATA_TX_RESULT_FLAG_MANAGED
                    : 0;
    if (queued_tx)
        data_tx_configure_job_cad_policy(tx, &job);
    data_tx_apply_cad_decision_flags(&job, cad_decision);
    if (daemon_tx_job_set_payload(&job, chunk, len) != 0) {
        daemon_debug_ctx(tx->log_ctx, "Abbruch: INVALID_PACKET");
        printf("[%s] DATA-TX abgebrochen: INVALID_PACKET\n", tag);
        return DAEMON_TX_OUTCOME_INVALID_PACKET;
    }

    result = daemon_data_tx_execute_job(tx, &job, send_fn);
    if (!queued_tx || daemon_tx_outcome_is_failure(result.outcome))
        daemon_radio_stats_record_tx_result(&ctrl->stats, result.tx_result);

    if (daemon_tx_outcome_is_failure(result.outcome)) {
        daemon_debug_ctx(tx->log_ctx, "Abbruch: %s",
                         tx_result_name(result.tx_result));
        printf("[%s] DATA-TX abgebrochen: %s\n", tag,
               tx_result_name(result.tx_result));
        return result.outcome;
    }

    daemon_debug_ctx(tx->log_ctx, "Chunk gesendet");
    return 0;
}

template<typename RadioT>
static DataTxDaemonContext<RadioT> daemon_data_tx_context(RadioController<RadioT> *ctrl)
{
    const char *log_ctx = "TX?";

    if (ctrl && ctrl->band == RADIO_BAND_433)
        log_ctx = "TX433";
    else if (ctrl && ctrl->band == RADIO_BAND_868)
        log_ctx = "TX868";

    DataTxDaemonContext<RadioT> ctx = {
        ctrl,
        log_ctx,
        daemon_tx_executor_default_send,
        NULL,
        DAEMON_TX_COMPLETION_SLOT_NONE,
        0,
        0u,
        daemon_tx_policy_busy_timeout_ticks(),
        (useconds_t)daemon_tx_policy_poll_interval_usec(),
        daemon_tx_policy_cad_wait_ticks(),
        daemon_tx_policy_cad_idle_stable_ticks(),
        (useconds_t)daemon_tx_policy_poll_interval_usec()
    };

    return ctx;
}

#endif
