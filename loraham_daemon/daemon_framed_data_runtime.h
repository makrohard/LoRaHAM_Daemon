#ifndef LORAHAM_DAEMON_FRAMED_DATA_RUNTIME_H
#define LORAHAM_DAEMON_FRAMED_DATA_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "client_slot.h"
#include "daemon_data_tx_runtime.h"
#include "event_loop.h"
#include "framed_data_tx.h"

/* --- Framed DATA runtime helpers ---------------------------------------- */

typedef int (*DaemonFramedTxResultEnabledFn)(void *ctx);
typedef uint16_t (*DaemonFramedTxNextSeqFn)(void *ctx);

template<typename RadioT>
static int send_framed_data_packet(uint8_t *payload,
                                   size_t len,
                                   void *ctx)
{
    return send_data_chunk<RadioT>(payload, len, 0, ctx);
}

void daemon_process_framed_data_slots(const char *tag,
                                      ClientSlot *slots,
                                      FramedDataTxState *states,
                                      int max_clients,
                                      const EventLoopReadySet *readfds,
                                      FramedDataTxPacketHandler handler,
                                      void *ctx,
                                      DaemonFramedTxResultEnabledFn result_enabled,
                                      DaemonFramedTxNextSeqFn next_seq);

#endif
