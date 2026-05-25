#ifndef BRIDGE_RX_H
#define BRIDGE_RX_H

#include "loraham_kiss_tnc.h"
#include "loraham_sock.h"

#include <stddef.h>
#include <stdint.h>

typedef int (*bridge_rx_should_stop_fn)(void);

void bridge_rx_set_should_stop(bridge_rx_should_stop_fn should_stop);

int bridge_rx_send_tnc2_to_kiss_client(int client_fd,
                                       const char *tnc2,
                                       lhkt_stats_t *stats);

int bridge_rx_handle_loraham_chunk(int client_fd,
                                   loraham_rx_state_t *rx_state,
                                   const uint8_t *buf,
                                   size_t len,
                                   lhkt_stats_t *stats);

int bridge_rx_handle_framed_chunk(int client_fd,
                                  loraham_framed_rx_state_t *frame_state,
                                  const uint8_t *buf,
                                  size_t len,
                                  lhkt_stats_t *stats);

#ifdef LHKT_TEST
int bridge_rx_test_wait_fd_writable(int fd);
#endif

#endif
