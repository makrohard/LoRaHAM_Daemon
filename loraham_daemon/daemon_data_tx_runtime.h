#ifndef LORAHAM_DAEMON_DATA_TX_RUNTIME_H
#define LORAHAM_DAEMON_DATA_TX_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "data_tx.h"
#include "daemon_log.h"
#include "daemon_radio_runtime.h"
#include "daemon_stats.h"
#include "daemon_tx.h"
#include "radio_controller.h"
#include "tx_result.h"

/* --- DATA TX runtime helpers -------------------------------------------- */

#define DATA_TX_CAD_MAX_WAIT_TICKS 300
#define DATA_TX_CAD_SLEEP_USEC 10000

template<typename RadioT>
struct DataTxDaemonContext {
    RadioController<RadioT> *ctrl;
    const char *log_ctx;
};

void daemon_data_tx_trace_message(void *ctx, const char *msg);
DataTxLog daemon_data_tx_log(const char *ctx);

template<typename RadioT>
static int data_tx_modem_status(DataTxDaemonContext<RadioT> *tx)
{
    RadioController<RadioT> *ctrl = tx->ctrl;

    if (!ctrl || !ctrl->radio || !radio_controller_ready(ctrl))
        return 0;

    return ctrl->radio->getModemStatus();
}

template<typename RadioT>
static int data_tx_wait_channel_free(DataTxDaemonContext<RadioT> *tx)
{
    RadioController<RadioT> *ctrl = tx->ctrl;
    int cad_wait = 0;

    if (!ctrl || ctrl->mode != RADIO_MODE_LORA)
        return 0;

    while (cad_wait < DATA_TX_CAD_MAX_WAIT_TICKS) {
        if ((data_tx_modem_status(tx) & 0x01) == 0)
            return 0;

        usleep(DATA_TX_CAD_SLEEP_USEC);
        cad_wait++;
    }

    return 1;
}

template<typename RadioT>
static int send_data_chunk(uint8_t *chunk, size_t len, size_t offset, void *ctx)
{
    DataTxDaemonContext<RadioT> *tx = (DataTxDaemonContext<RadioT> *)ctx;
    RadioController<RadioT> *ctrl = tx->ctrl;
    const char *tag = radio_controller_tag(ctrl);
    int band = radio_controller_band_number(ctrl);

    if (!radio_controller_ready(ctrl)) {
        daemon_debug_ctx(tx->log_ctx, "Radio nicht bereit");
        printf("[%s] DATA-TX abgebrochen: RADIO_NOT_READY\n", tag);
        return 1;
    }

    // CAD guard: LoRa only.
    if (ctrl->mode == RADIO_MODE_LORA)
        daemon_debug_ctx(tx->log_ctx, "CAD prüfen");

    if (data_tx_wait_channel_free(tx)) {
        daemon_radio_stats_record_cad_timeout(&ctrl->stats);
        daemon_debug_ctx(tx->log_ctx, "CAD Timeout");
        printf("[%s] CAD-Timeout: Kanal dauerhaft belegt, Paket verworfen\n", tag);
        printf("[%s] DATA-TX abgebrochen: %s\n", tag,
               tx_result_name(TX_RESULT_CAD_TIMEOUT));
        return 1;
    }

    daemon_debug_ctx(tx->log_ctx, "Chunk %zu Byte Offset %zu", len, offset);

    daemon_radio_runtime_led(ctrl, 1);
    TxResult result = lora_send(chunk, len, band);
    daemon_radio_stats_record_tx_result(&ctrl->stats, result);
    daemon_radio_runtime_led(ctrl, 0);

    if (!tx_result_is_ok(result)) {
        daemon_debug_ctx(tx->log_ctx, "Abbruch: %s", tx_result_name(result));
        printf("[%s] DATA-TX abgebrochen: %s\n", tag,
               tx_result_name(result));
        return 1;
    }

    daemon_debug_ctx(tx->log_ctx, "Chunk gesendet");
    return 0;
}

template<typename RadioT>
static DataTxDaemonContext<RadioT> daemon_data_tx_context(RadioController<RadioT> *ctrl)
{
    const char *log_ctx = "TX?";

    if (ctrl && ctrl->band == RADIO_BAND_433)
        log_ctx = "TX433";
    else if (ctrl && ctrl->band == RADIO_BAND_868)
        log_ctx = "TX868";

    DataTxDaemonContext<RadioT> ctx = {
        ctrl,
        log_ctx
    };

    return ctx;
}

#endif
