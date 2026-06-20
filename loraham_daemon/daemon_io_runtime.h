#ifndef LORAHAM_DAEMON_IO_RUNTIME_H
#define LORAHAM_DAEMON_IO_RUNTIME_H

#include "client_slot.h"
#include "daemon_protocol.h"
#include "event_loop.h"
#include "framed_data_tx.h"
#include "radio_channel.h"

/* --- Daemon I/O runtime state ------------------------------------------- */

extern int data433_fd;
extern int data868_fd;
extern int data433_framed_fd;
extern int data868_framed_fd;
extern int conf433_fd;
extern int conf868_fd;

extern ClientSlot client_data433_slots[MAX_CLIENTS];
extern ClientSlot client_data868_slots[MAX_CLIENTS];
extern ClientSlot client_data433_framed_slots[MAX_CLIENTS];
extern ClientSlot client_data868_framed_slots[MAX_CLIENTS];
extern FramedDataTxState client_data433_framed_states[MAX_CLIENTS];
extern FramedDataTxState client_data868_framed_states[MAX_CLIENTS];
extern ClientSlot client_conf433_slots[MAX_CLIENTS];
extern ClientSlot client_conf868_slots[MAX_CLIENTS];

extern RadioChannelIo channel_433;
extern RadioChannelIo channel_868;

/* --- Daemon I/O lifecycle ----------------------------------------------- */

void daemon_io_startup_cleanup(void);
void daemon_io_shutdown_cleanup(void);
void daemon_io_init(void);
void daemon_io_sync_event_fds(EventLoopSet *event_set);

#endif
