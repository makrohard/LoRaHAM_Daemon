#ifndef BRIDGE_TX_QUEUE_H
#define BRIDGE_TX_QUEUE_H

#include "bridge_conf.h"
#include "loraham_kiss_tnc.h"

#include <stddef.h>
#include <stdint.h>

#define LHKT_TX_QUEUE_MAX 16

#define BRIDGE_TX_DECISION_WAIT  0
#define BRIDGE_TX_DECISION_SEND  1
#define BRIDGE_TX_DECISION_DROP  2

typedef struct {
    uint8_t packet[LHKT_LORAHAM_TX_MAX];
    size_t packet_len;
    long queued_ms;
    long tx_wait_start_ms;
} bridge_tx_item_t;

typedef struct {
    bridge_tx_item_t items[LHKT_TX_QUEUE_MAX];
    size_t head;
    size_t count;
} bridge_tx_queue_t;

void bridge_tx_queue_init(bridge_tx_queue_t *queue);

int bridge_tx_queue_empty(const bridge_tx_queue_t *queue);

bridge_tx_item_t *bridge_tx_queue_head(bridge_tx_queue_t *queue);

void bridge_tx_queue_pop(bridge_tx_queue_t *queue);

int bridge_tx_queue_push(bridge_tx_queue_t *queue,
                         const lhkt_config_t *cfg,
                         const uint8_t *packet,
                         size_t packet_len);

int bridge_tx_head_decision(const lhkt_config_t *cfg,
                            bridge_conf_state_t *conf_state,
                            bridge_tx_item_t *item,
                            long now);

#ifdef LHKT_TEST
int lhkt_test_bridge_tx_decision(int tx_busy,
                                  long queued_age_ms,
                                  long tx_wait_age_ms);
#endif

#endif
