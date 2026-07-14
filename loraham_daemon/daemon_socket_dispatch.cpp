#include "daemon_socket_dispatch.h"

#include "daemon_data_tx_runtime.h"
#include "daemon_framed_data_runtime.h"
#include "daemon_io_runtime.h"
#include "daemon_radio_selection.h"
#include "daemon_socket_runtime.h"
#include "data_tx.h"

/* --- Socket dispatch orchestration -------------------------------------- */

void daemon_process_ready_sockets(ConfigDispatchContext *config_433_ctx,
                                         ConfigDispatchContext *config_868_ctx,
                                         DataTxDaemonContext *data_tx_433_ctx,
                                         DataTxDaemonContext *data_tx_868_ctx,
                                         const EventLoopReadySet *readfds,
                                         uint8_t *buf)
{
    if (daemon_radio_433_enabled()) {
        daemon_accept_channel_logged(&channel_433, readfds, "CLIENT433");
        data_tx_process_slots("433", client_data433_slots, MAX_CLIENTS,
                              readfds, send_data_chunk, data_tx_433_ctx,
                              daemon_data_tx_log("TX433"));
        daemon_process_framed_data_slots("TX433F",
                                         client_data433_framed_slots,
                                         client_data433_framed_states,
                                         MAX_CLIENTS,
                                         readfds,
                                         send_framed_data_packet,
                                         data_tx_433_ctx,
                                         daemon_data_tx_result_enabled,
                                         daemon_data_tx_next_result_seq,
                                         daemon_data_tx_result_deferred,
                                         daemon_data_tx_set_completion_target,
                                         daemon_data_tx_managed_flag);
        daemon_drain_framed_tx_completions("TX433F",
                                           433,
                                           client_data433_framed_slots,
                                           MAX_CLIENTS);
        config_dispatch_context(config_433_ctx, MAX_CLIENTS, readfds, buf);
        daemon_flush_channel_logged(&channel_433, readfds, "CLIENT433");
    }

    if (daemon_radio_868_enabled()) {
        daemon_accept_channel_logged(&channel_868, readfds, "CLIENT868");
        data_tx_process_slots("868", client_data868_slots, MAX_CLIENTS,
                              readfds, send_data_chunk, data_tx_868_ctx,
                              daemon_data_tx_log("TX868"));
        daemon_process_framed_data_slots("TX868F",
                                         client_data868_framed_slots,
                                         client_data868_framed_states,
                                         MAX_CLIENTS,
                                         readfds,
                                         send_framed_data_packet,
                                         data_tx_868_ctx,
                                         daemon_data_tx_result_enabled,
                                         daemon_data_tx_next_result_seq,
                                         daemon_data_tx_result_deferred,
                                         daemon_data_tx_set_completion_target,
                                         daemon_data_tx_managed_flag);
        daemon_drain_framed_tx_completions("TX868F",
                                           868,
                                           client_data868_framed_slots,
                                           MAX_CLIENTS);
        config_dispatch_context(config_868_ctx, MAX_CLIENTS, readfds, buf);
        daemon_flush_channel_logged(&channel_868, readfds, "CLIENT868");
    }
}
