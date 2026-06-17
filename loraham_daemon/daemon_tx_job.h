#ifndef LORAHAM_DAEMON_TX_JOB_H
#define LORAHAM_DAEMON_TX_JOB_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "daemon_tx_outcome.h"
#include "framed_data.h"

/* --- Interner TX-Auftrag ------------------------------------------------- */

typedef struct {
    int band;
    int tx_mode;
    uint16_t seq;
    uint8_t flags;
    size_t payload_len;
    uint8_t payload[FRAMED_DATA_MAX_RF_PAYLOAD];
} DaemonTxJob;

typedef struct {
    uint16_t seq;
    uint8_t flags;
    DaemonTxOutcome outcome;
    TxResult tx_result;
    uint8_t framed_status;
} DaemonTxJobResult;

static inline void daemon_tx_job_init(DaemonTxJob *job,
                                      int band,
                                      int tx_mode,
                                      uint16_t seq)
{
    if (!job)
        return;

    memset(job, 0, sizeof(*job));
    job->band = band;
    job->tx_mode = tx_mode;
    job->seq = seq;
}

static inline int daemon_tx_job_set_payload(DaemonTxJob *job,
                                            const uint8_t *payload,
                                            size_t len)
{
    if (!job || (!payload && len > 0))
        return -1;

    if (len > FRAMED_DATA_MAX_RF_PAYLOAD)
        return -1;

    job->payload_len = len;
    if (len > 0)
        memcpy(job->payload, payload, len);

    return 0;
}

static inline void daemon_tx_job_result_init(DaemonTxJobResult *result,
                                             const DaemonTxJob *job,
                                             DaemonTxOutcome outcome)
{
    if (!result)
        return;

    memset(result, 0, sizeof(*result));
    result->seq = job ? job->seq : 0;
    result->flags = job ? job->flags : 0;
    result->outcome = outcome;
    result->tx_result = daemon_tx_outcome_to_tx_result(outcome);
    result->framed_status = daemon_tx_outcome_to_framed_status(outcome);
}

#endif
