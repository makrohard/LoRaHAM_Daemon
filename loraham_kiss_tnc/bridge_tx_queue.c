#define _POSIX_C_SOURCE 200809L
#include "bridge_tx_queue.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Outgoing LoRaHAM TX queue and TX-busy policy decisions.
 * Daemon 110 owns the final CAD gate before RF transmit.
 * Actual socket writes and RX restore are kept in bridge.c.
 */

static long bridge_tx_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

void bridge_tx_queue_init(bridge_tx_queue_t *queue)
{
    if (!queue) {
        return;
    }

    memset(queue, 0, sizeof(*queue));
}

static size_t bridge_tx_queue_limit(const lhkt_config_t *cfg)
{
    if (!cfg || cfg->tx_queue_len < 1) {
        return 1;
    }

    if (cfg->tx_queue_len > LHKT_TX_QUEUE_MAX) {
        return LHKT_TX_QUEUE_MAX;
    }

    return (size_t)cfg->tx_queue_len;
}

int bridge_tx_queue_empty(const bridge_tx_queue_t *queue)
{
    return !queue || queue->count == 0;
}

bridge_tx_item_t *bridge_tx_queue_head(bridge_tx_queue_t *queue)
{
    if (!queue || queue->count == 0) {
        return NULL;
    }

    return &queue->items[queue->head];
}

void bridge_tx_queue_pop(bridge_tx_queue_t *queue)
{
    if (!queue || queue->count == 0) {
        return;
    }

    memset(&queue->items[queue->head], 0, sizeof(queue->items[queue->head]));
    queue->head = (queue->head + 1) % LHKT_TX_QUEUE_MAX;
    queue->count--;
}

int bridge_tx_queue_push(bridge_tx_queue_t *queue,
                         const lhkt_config_t *cfg,
                         const uint8_t *packet,
                         size_t packet_len)
{
    bridge_tx_item_t *item;
    size_t index;

    if (!queue || !cfg || !packet || packet_len == 0 ||
        packet_len > LHKT_LORAHAM_TX_MAX) {
        return LHKT_ERR_FORMAT;
    }

    if (queue->count >= bridge_tx_queue_limit(cfg)) {
        return LHKT_ERR_NOSPACE;
    }

    index = (queue->head + queue->count) % LHKT_TX_QUEUE_MAX;
    item = &queue->items[index];
    memset(item, 0, sizeof(*item));

    memcpy(item->packet, packet, packet_len);
    item->packet_len = packet_len;
    item->queued_ms = bridge_tx_now_ms();
    queue->count++;

    return LHKT_OK;
}

int bridge_tx_head_decision(const lhkt_config_t *cfg,
                            bridge_conf_state_t *conf_state,
                            bridge_tx_item_t *item,
                            long now)
{
    if (!cfg || !item) {
        return BRIDGE_TX_DECISION_DROP;
    }

    if (cfg->tx_packet_ttl_ms > 0 &&
        now - item->queued_ms >= cfg->tx_packet_ttl_ms) {
        printf("[TXQ] Drop expired packet len=%zu\n", item->packet_len);
        return BRIDGE_TX_DECISION_DROP;
    }

    if (conf_state && conf_state->tx_busy) {
        if (item->tx_wait_start_ms == 0) {
            item->tx_wait_start_ms = now;
        }

        if (now - item->tx_wait_start_ms >= cfg->tx_busy_timeout_ms) {
            printf("[TXQ] Drop packet after TX busy timeout len=%zu\n",
                   item->packet_len);
            return BRIDGE_TX_DECISION_DROP;
        }

        return BRIDGE_TX_DECISION_WAIT;
    }

    item->tx_wait_start_ms = 0;

    /*
     * Do not do bridge-side CAD politeness here.  Daemon 110 performs
     * the final CAD gate immediately before RF TX and can return an
     * asynchronous framed ERROR.  Without TX/error correlation in the
     * framed protocol, guessing/requeueing here would be unsafe.
     */
    return BRIDGE_TX_DECISION_SEND;
}

#ifdef LHKT_TEST
int lhkt_test_bridge_tx_decision(int tx_busy,
                                  int cad_busy,
                                  int cad_ignore,
                                  long queued_age_ms,
                                  long tx_wait_age_ms,
                                  long cad_wait_age_ms,
                                  long cad_idle_age_ms,
                                  int cad_was_busy)
{
    lhkt_config_t cfg;
    bridge_conf_state_t conf_state;
    bridge_tx_item_t item;
    long now;

    lhkt_config_defaults(&cfg);
    cfg.cad_ignore = cad_ignore;
    bridge_conf_state_init(&conf_state);
    memset(&item, 0, sizeof(item));

    conf_state.tx_busy = tx_busy;
    conf_state.cad_busy = cad_busy;
    item.packet_len = 10;
    item.cad_was_busy = cad_was_busy;

    now = bridge_tx_now_ms();
    item.queued_ms = now - (queued_age_ms >= 0 ? queued_age_ms : 0);

    if (tx_wait_age_ms >= 0) {
        item.tx_wait_start_ms = now - tx_wait_age_ms;
    }

    if (cad_wait_age_ms >= 0) {
        item.cad_wait_start_ms = now - cad_wait_age_ms;
    }

    if (cad_idle_age_ms >= 0) {
        item.cad_idle_since_ms = now - cad_idle_age_ms;
    }

    return bridge_tx_head_decision(&cfg, &conf_state, &item, now);
}
#endif
