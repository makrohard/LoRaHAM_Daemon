#include "daemon_socket_dispatch.h"

#include "daemon_data_tx_runtime.h"
#include "daemon_framed_data_runtime.h"
#include "daemon_io_runtime.h"
#include "daemon_radio_selection.h"
#include "daemon_socket_runtime.h"
#include "data_tx.h"

/* --- Socket dispatch orchestration -------------------------------------- */

void daemon_process_ready_sockets(ConfigDispatchContext<SX1278> *config_433_ctx,
                                         ConfigDispatchContext<RFM95> *config_868_ctx,
                                         DataTxDaemonContext<SX1278> *data_tx_433_ctx,
                                         DataTxDaemonContext<RFM95> *data_tx_868_ctx,
                                         const EventLoopReadySet *readfds,
                                         uint8_t *buf)
{
    if (daemon_radio_433_enabled()) {
        daemon_accept_channel_logged(&channel_433, readfds, "CLIENT433");
        data_tx_process_slots("433", client_data433_slots, MAX_CLIENTS,
                              readfds, send_data_chunk<SX1278>, data_tx_433_ctx,
                              daemon_data_tx_log("TX433"));
        daemon_process_framed_data_slots("TX433F",
                                         client_data433_framed_slots,
                                         client_data433_framed_states,
                                         MAX_CLIENTS,
                                         readfds,
                                         send_framed_data_packet<SX1278>,
                                         data_tx_433_ctx,
                                         daemon_data_tx_result_enabled<SX1278>,
                                         daemon_data_tx_next_result_seq<SX1278>);
        config_dispatch_context<SX1278>(config_433_ctx, MAX_CLIENTS, readfds, buf);
        daemon_flush_channel_logged(&channel_433, readfds, "CLIENT433");
    }

    if (daemon_radio_868_enabled()) {
        daemon_accept_channel_logged(&channel_868, readfds, "CLIENT868");
        data_tx_process_slots("868", client_data868_slots, MAX_CLIENTS,
                              readfds, send_data_chunk<RFM95>, data_tx_868_ctx,
                              daemon_data_tx_log("TX868"));
        daemon_process_framed_data_slots("TX868F",
                                         client_data868_framed_slots,
                                         client_data868_framed_states,
                                         MAX_CLIENTS,
                                         readfds,
                                         send_framed_data_packet<RFM95>,
                                         data_tx_868_ctx,
                                         daemon_data_tx_result_enabled<RFM95>,
                                         daemon_data_tx_next_result_seq<RFM95>);
        config_dispatch_context<RFM95>(config_868_ctx, MAX_CLIENTS, readfds, buf);
        daemon_flush_channel_logged(&channel_868, readfds, "CLIENT868");
    }
}
