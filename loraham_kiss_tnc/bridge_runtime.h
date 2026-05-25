#ifndef BRIDGE_RUNTIME_H
#define BRIDGE_RUNTIME_H

#include "loraham_kiss_tnc.h"

#include <sys/select.h>

void bridge_runtime_reset_stop_requested(void);
int bridge_runtime_should_stop(void);
void bridge_runtime_install_signal_handlers(void);

int bridge_runtime_add_fd_to_set(int fd, fd_set *set, int *max_fd);
int bridge_runtime_set_fd_nonblocking(int fd);

int bridge_runtime_sleep_ms(int ms);
long bridge_runtime_now_ms(void);

#ifdef LHKT_TEST
void bridge_runtime_test_reset_hooks(void);

void lhkt_test_bridge_reset_stop(void);
void lhkt_test_bridge_request_stop(void);
int lhkt_test_bridge_should_stop(void);

int lhkt_test_add_fd_to_set(int fd);
int lhkt_test_bridge_wait_fd_writable(int fd);
int lhkt_test_bridge_sleep_ms(int ms);
size_t lhkt_test_bridge_sleep_call_count(void);
#endif

#endif
