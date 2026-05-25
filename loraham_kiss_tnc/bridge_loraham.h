#ifndef BRIDGE_LORAHAM_H
#define BRIDGE_LORAHAM_H

#include "bridge_conf.h"
#include "bridge_tx_queue.h"
#include "loraham_kiss_tnc.h"
#include "loraham_sock.h"
#include "kiss.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int bridge_loraham_connect_conf_socket(const lhkt_config_t *cfg,
                                       bridge_conf_state_t *state);

int bridge_loraham_connect_data_socket(const lhkt_config_t *cfg);

int bridge_loraham_send_initial_config(const lhkt_config_t *cfg,
                                       int conf_fd);

int bridge_loraham_tx_queue_drain(const lhkt_config_t *cfg,
                                  lhkt_stats_t *stats,
                                  bridge_tx_queue_t *queue,
                                  int data_fd,
                                  int conf_fd,
                                  bridge_conf_state_t *conf_state);

int bridge_loraham_should_reconnect_data_socket(int ret);
int bridge_loraham_should_reconnect_conf_socket(int ret);

void bridge_loraham_disconnect_conf_socket(int *conf_fd,
                                           bridge_conf_state_t *conf_state,
                                           lhkt_stats_t *stats,
                                           const char *reason);

void bridge_loraham_disconnect_data_socket(int *data_fd,
                                           loraham_rx_state_t *rx_state,
                                           lhkt_stats_t *stats,
                                           const char *reason);

#ifdef LHKT_TEST
void lhkt_test_bridge_reset_tx_hooks(void);
void lhkt_test_bridge_set_config_results(const int *results, size_t count);
size_t lhkt_test_bridge_config_call_count(void);
double lhkt_test_bridge_config_freq_at(size_t index);
void lhkt_test_bridge_set_write_result(ssize_t result);
size_t lhkt_test_bridge_write_call_count(void);

int lhkt_test_bridge_send_initial_config(const lhkt_config_t *cfg);
int lhkt_test_bridge_wait_tx_complete_events(const char *events);
int lhkt_test_handle_kiss_frame(const kiss_frame_t *kiss_frame,
                                kiss_params_t *kiss_params,
                                const lhkt_config_t *cfg,
                                lhkt_stats_t *stats,
                                int data_fd);
int lhkt_test_bridge_send_packet_without_conf(uint64_t *tx,
                                              uint64_t *drops,
                                              size_t *write_calls);
int lhkt_test_bridge_send_packet_without_tx_confirm(uint64_t *tx,
                                                     uint64_t *unconfirmed,
                                                     size_t *write_calls);
int lhkt_test_bridge_drain_waits_for_data_socket(size_t *queue_depth,
                                                 uint64_t *drops);
int lhkt_test_bridge_drain_waits_for_conf_socket(size_t *queue_depth,
                                                 size_t *write_calls,
                                                 uint64_t *drops);
int lhkt_test_bridge_drain_reads_pending_conf(size_t *queue_depth,
                                             int *tx_busy,
                                             size_t *write_calls);
int lhkt_test_bridge_drain_pops_written_restore_failure(size_t *queue_depth,
                                                        uint64_t *restore_failures,
                                                        size_t *write_calls);
int lhkt_test_bridge_should_reconnect_data_socket(int ret);
int lhkt_test_bridge_should_reconnect_conf_socket(int ret);
void lhkt_test_bridge_disconnect_data_socket(int *data_fd,
                                             loraham_rx_state_t *rx_state,
                                             lhkt_stats_t *stats);
#endif

#endif
