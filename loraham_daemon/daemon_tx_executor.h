#ifndef LORAHAM_DAEMON_TX_EXECUTOR_H
#define LORAHAM_DAEMON_TX_EXECUTOR_H

#include <stddef.h>
#include <stdint.h>

#include "daemon_tx.h"
#include "daemon_tx_job.h"
#include "daemon_tx_outcome.h"

/* --- TX-Ausfuehrung ----------------------------------------------- */

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

static inline DaemonTxJobResult daemon_tx_execute_job(const DaemonTxJob *job)
{
    return daemon_tx_execute_job_with_sender(job,
                                            daemon_tx_executor_default_send,
                                            NULL);
}

#endif
