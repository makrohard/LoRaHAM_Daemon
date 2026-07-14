#ifndef LORAHAM_DAEMON_SOCKET_DISPATCH_H
#define LORAHAM_DAEMON_SOCKET_DISPATCH_H

#include <stdint.h>

#include "config_dispatch.h"
#include "daemon_data_tx_runtime.h"
#include "event_loop.h"

/* --- Socket dispatch orchestration -------------------------------------- */

void daemon_process_ready_sockets(ConfigDispatchContext *config_433_ctx,
                                  ConfigDispatchContext *config_868_ctx,
                                  DataTxDaemonContext *data_tx_433_ctx,
                                  DataTxDaemonContext *data_tx_868_ctx,
                                  const EventLoopReadySet *readfds,
                                  uint8_t *buf);

#endif
