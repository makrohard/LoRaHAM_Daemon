#ifndef LORAHAM_DAEMON_TX_COMPLETION_H
#define LORAHAM_DAEMON_TX_COMPLETION_H

#include <stddef.h>
#include <stdint.h>

#include "daemon_tx_job.h"
#include "framed_data.h"

/* --- TX-Abschlussmeldung ------------------------------------------------- */

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

#endif
