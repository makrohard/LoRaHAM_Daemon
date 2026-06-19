#ifndef LORAHAM_DAEMON_TX_EXECUTOR_H
#define LORAHAM_DAEMON_TX_EXECUTOR_H

#include <stddef.h>
#include <stdint.h>

#include "daemon_tx.h"
#include "daemon_tx_job.h"
#include "daemon_tx_outcome.h"

/* --- TX-Ausfuehrung ----------------------------------------------- */

typedef enum {
    DAEMON_TX_CAD_PROBE_UNAVAILABLE = -1,
    DAEMON_TX_CAD_PROBE_FREE = 0,
    DAEMON_TX_CAD_PROBE_BUSY = 1
} DaemonTxCadProbeState;

typedef int (*DaemonTxCadProbeFn)(int band, void *ctx);
typedef void (*DaemonTxSleepFn)(uint32_t usec, void *ctx);

typedef TxResult (*DaemonTxSendFn)(uint8_t *payload,
                                   size_t len,
                                   int band,
                                   void *ctx);

static inline TxResult daemon_tx_executor_default_send(uint8_t *payload,
                                                       size_t len,
                                                       int band,
                                                       void *ctx)
{
    (void)ctx;

    return lora_send(payload, len, band);
}

static inline DaemonTxJobResult daemon_tx_execute_job_with_sender(const DaemonTxJob *job,
                                                                 DaemonTxSendFn send_fn,
                                                                 void *send_ctx)
{
    DaemonTxJobResult result;
    TxResult tx_result;

    if (!job) {
        daemon_tx_job_result_init(&result,
                                  NULL,
                                  DAEMON_TX_OUTCOME_INVALID_PACKET);
        return result;
    }

    if (!send_fn) {
        daemon_tx_job_result_init(&result,
                                  job,
                                  DAEMON_TX_OUTCOME_RADIO_ERROR);
        return result;
    }

    tx_result = send_fn((uint8_t *)job->payload,
                        job->payload_len,
                        job->band,
                        send_ctx);

    daemon_tx_job_result_init(&result,
                              job,
                              daemon_tx_outcome_from_tx_result(tx_result));
    result.tx_result = tx_result;
    return result;
}

static inline DaemonTxJobResult daemon_tx_execute_job_with_sender_and_cad(const DaemonTxJob *job,
                                                                          DaemonTxSendFn send_fn,
                                                                          void *send_ctx,
                                                                          DaemonTxCadProbeFn cad_probe_fn,
                                                                          void *cad_ctx,
                                                                          DaemonTxSleepFn sleep_fn,
                                                                          void *sleep_ctx)
{
    DaemonTxJob run_job;
    uint32_t idle_ticks = 0;
    uint32_t elapsed_ticks = 0;

    if (!job)
        return daemon_tx_execute_job_with_sender(job, send_fn, send_ctx);

    run_job = *job;

    if (!run_job.cad_enabled || !cad_probe_fn)
        return daemon_tx_execute_job_with_sender(&run_job, send_fn, send_ctx);

    uint32_t stable_ticks = run_job.cad_idle_stable_ticks;
    if (stable_ticks == 0)
        stable_ticks = 1;

    for (;;) {
        int cad_state = cad_probe_fn(run_job.band, cad_ctx);

        if (cad_state == DAEMON_TX_CAD_PROBE_FREE) {
            idle_ticks++;
            if (idle_ticks >= stable_ticks)
                break;
        } else if (cad_state == DAEMON_TX_CAD_PROBE_BUSY) {
            idle_ticks = 0;
        } else {
            DaemonTxJobResult result;
            daemon_tx_job_result_init(&result,
                                      &run_job,
                                      DAEMON_TX_OUTCOME_RADIO_ERROR);
            return result;
        }

        elapsed_ticks++;
        if (run_job.cad_wait_ticks > 0 &&
            elapsed_ticks >= run_job.cad_wait_ticks) {
            if (run_job.cad_send_after_timeout) {
                run_job.flags |= FRAMED_DATA_TX_RESULT_FLAG_CAD_TIMEOUT;
                break;
            }

            DaemonTxJobResult result;
            daemon_tx_job_result_init(&result,
                                      &run_job,
                                      DAEMON_TX_OUTCOME_CHANNEL_BUSY);
            return result;
        }

        if (sleep_fn && run_job.cad_poll_interval_usec > 0)
            sleep_fn(run_job.cad_poll_interval_usec, sleep_ctx);
    }

    return daemon_tx_execute_job_with_sender(&run_job, send_fn, send_ctx);
}

static inline DaemonTxJobResult daemon_tx_execute_job(const DaemonTxJob *job)
{
    return daemon_tx_execute_job_with_sender(job,
                                            daemon_tx_executor_default_send,
                                            NULL);
}

#endif
