#include "bridge_kiss.h"

#include "ax25.h"
#include "loraham_sock.h"
#include "tnc2.h"

#include <stdio.h>

/*
 * KISS ingress path.
 * Converts outgoing KISS/AX.25 UI frames to LoRaHAM APRS packets
 * and enqueues them for the bridge TX lifecycle.
 */

static int bridge_kiss_handle_data_frame(const kiss_frame_t *kiss_frame,
                                         const lhkt_config_t *cfg,
                                         lhkt_stats_t *stats,
                                         bridge_tx_queue_t *tx_queue)
{
    ax25_frame_t ax25;
    char tnc2[LHKT_TNC2_MAX_LINE];
    size_t tnc2_len;
    int ret;

    if (!kiss_frame || !cfg) {
        return LHKT_ERR;
    }

    ret = ax25_decode_ui(kiss_frame->data, kiss_frame->data_len, &ax25);
    if (ret != LHKT_OK) {
        if (stats) {
            stats->ax25_drop++;
        }

        printf("[AX25] Drop invalid/non-UI frame: err=%d len=%zu\n",
               ret,
               kiss_frame->data_len);
        return ret;
    }

    if (stats) {
        stats->ax25_rx++;
    }

    ret = tnc2_format_line(&ax25, tnc2, sizeof(tnc2), &tnc2_len);
    if (ret != LHKT_OK) {
        if (stats) {
            stats->tnc2_drop++;
        }

        printf("[TNC2] Drop frame, format failed: err=%d\n", ret);
        return ret;
    }

    if (stats) {
        stats->tnc2_tx++;
    }

    printf("[TNC2 TX] %s\n", tnc2);

    {
        uint8_t lora_pkt[LHKT_LORAHAM_TX_MAX];
        size_t lora_len = 0;

        ret = loraham_build_aprs_packet(tnc2,
                                        lora_pkt,
                                        sizeof(lora_pkt),
                                        &lora_len);
        if (ret != LHKT_OK) {
            if (stats) {
                stats->loraham_drop++;

                if (ret == LHKT_ERR_LONG) {
                    stats->tx_drop_oversize++;
                }
            }

            printf("[LoRaHAM] TX drop: err=%d tnc2_len=%zu\n",
                   ret,
                   tnc2_len);
            return ret;
        }

        if (cfg->rx_only) {
            printf("[LoRaHAM] RX-only: TX suppressed packet len=%zu\n", lora_len);
            return LHKT_OK;
        }

        ret = bridge_tx_queue_push(tx_queue, cfg, lora_pkt, lora_len);
        if (ret != LHKT_OK) {
            if (stats) {
                stats->loraham_drop++;
            }

            printf("[TXQ] TX drop: queue full or invalid packet len=%zu\n", lora_len);
            return ret;
        }

        printf("[TXQ] Queued packet len=%zu depth=%zu\n",
               lora_len,
               tx_queue ? tx_queue->count : 0);
    }

    return LHKT_OK;
}

int bridge_kiss_handle_frame(const kiss_frame_t *kiss_frame,
                             kiss_params_t *kiss_params,
                             const lhkt_config_t *cfg,
                             lhkt_stats_t *stats,
                             bridge_tx_queue_t *tx_queue)
{
    int ret;

    if (!kiss_frame || !cfg) {
        return LHKT_ERR;
    }

    kiss_handle_command(kiss_params, kiss_frame);

    if (kiss_frame->command == KISS_CMD_DATA) {
        printf("[KISS] Data frame: port=%u len=%zu\n",
               kiss_frame->port,
               kiss_frame->data_len);

        if (kiss_frame->port != 0) {
            if (stats) {
                stats->kiss_drop++;
            }

            printf("[KISS] Drop unsupported port: %u\n",
                   kiss_frame->port);
            return LHKT_ERR_UNSUPPORTED;
        }

        ret = bridge_kiss_handle_data_frame(kiss_frame,
                                            cfg,
                                            stats,
                                            tx_queue);
        return ret;
    }

    if (cfg->verbose) {
        printf("[KISS] Command: port=%u cmd=%u len=%zu\n",
               kiss_frame->port,
               kiss_frame->command,
               kiss_frame->data_len);
    }

    return LHKT_OK;
}
