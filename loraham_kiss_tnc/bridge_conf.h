#ifndef BRIDGE_CONF_H
#define BRIDGE_CONF_H

#include "loraham_kiss_tnc.h"

#include <stddef.h>
#include <stdint.h>

#define LHKT_CONF_LINE_MAX 256

typedef struct {
    int tx_busy;
    int cad_busy;
    int radio_ready;
    int have_status;
    unsigned long tx_seq;
    unsigned long tx_busy_seq;
    unsigned long tx_idle_seq;
    unsigned long cad_seq;
    unsigned long cad_busy_seq;
    unsigned long cad_idle_seq;
    char line[LHKT_CONF_LINE_MAX];
    size_t line_len;
} bridge_conf_state_t;

void bridge_conf_state_init(bridge_conf_state_t *state);

void bridge_conf_feed(bridge_conf_state_t *state,
                      const uint8_t *buf,
                      size_t len);

int bridge_conf_read_available(int conf_fd,
                               bridge_conf_state_t *state);

int bridge_conf_wait_read(int conf_fd,
                          bridge_conf_state_t *state,
                          long wait_ms);

void bridge_conf_collect_cad_stats(const bridge_conf_state_t *state,
                                   lhkt_stats_t *stats,
                                   unsigned long *last_busy_seq,
                                   unsigned long *last_idle_seq);

#ifdef LHKT_TEST
int lhkt_test_bridge_conf_feed_events(const char *text,
                                      int *tx_busy,
                                      int *cad_busy,
                                      int *radio_ready,
                                      int *have_status,
                                      unsigned long *tx_seq,
                                      unsigned long *tx_busy_seq,
                                      unsigned long *tx_idle_seq,
                                      unsigned long *cad_seq,
                                      unsigned long *cad_busy_seq,
                                      unsigned long *cad_idle_seq);

int lhkt_test_bridge_conf_feed(const char *text,
                               int *tx_busy,
                               int *cad_busy,
                               int *radio_ready,
                               int *have_status);
#endif

#endif
