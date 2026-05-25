#ifndef BRIDGE_KISS_H
#define BRIDGE_KISS_H

#include "bridge_tx_queue.h"
#include "kiss.h"
#include "loraham_kiss_tnc.h"

int bridge_kiss_handle_frame(const kiss_frame_t *kiss_frame,
                             kiss_params_t *kiss_params,
                             const lhkt_config_t *cfg,
                             lhkt_stats_t *stats,
                             bridge_tx_queue_t *tx_queue);

#endif
