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
#include "daemon_tx_outcome.h"
#include "radio_cad.h"
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
static int data_tx_probe_channel_busy(DataTxDaemonContext<RadioT> *tx)
{
    RadioController<RadioT> *ctrl = tx ? tx->ctrl : NULL;
    RadioCadProbeResult probe;

    if (!ctrl || ctrl->mode != RADIO_MODE_LORA)
        return 0;

    probe = radio_cad_probe(ctrl);
    return probe.status == RADIO_CAD_PROBE_BUSY;
}

template<typename RadioT>
static int data_tx_wait_channel_free(DataTxDaemonContext<RadioT> *tx)
{
    RadioController<RadioT> *ctrl = tx ? tx->ctrl : NULL;
    int cad_wait = 0;

    if (!ctrl || ctrl->mode != RADIO_MODE_LORA)
        return 0;

    if (ctrl->tx_mode == RADIO_TX_MODE_RAW)
        return data_tx_probe_channel_busy(tx);

    while (cad_wait < DATA_TX_CAD_MAX_WAIT_TICKS) {
        if (!data_tx_probe_channel_busy(tx))
            return 0;

        usleep(DATA_TX_CAD_SLEEP_USEC);
        cad_wait++;
    }

    return 1;
}

template<typename RadioT>
static int daemon_data_tx_result_enabled(void *ctx)
{
    DataTxDaemonContext<RadioT> *tx =
        (DataTxDaemonContext<RadioT> *)ctx;

    return tx && tx->ctrl && tx->ctrl->tx_result_active.load();
}

template<typename RadioT>
static uint16_t daemon_data_tx_next_result_seq(void *ctx)
{
    DataTxDaemonContext<RadioT> *tx =
        (DataTxDaemonContext<RadioT> *)ctx;

    if (!tx || !tx->ctrl)
        return 0;

    tx->ctrl->tx_result_seq++;
    return tx->ctrl->tx_result_seq;
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
        return DAEMON_TX_OUTCOME_RADIO_NOT_READY;
    }

    // CAD guard: LoRa only.
    if (ctrl->mode == RADIO_MODE_LORA)
        daemon_debug_ctx(tx->log_ctx, "CAD prüfen");

    if (data_tx_wait_channel_free(tx)) {
        TxResult busy_result = ctrl->tx_mode == RADIO_TX_MODE_RAW ?
                               TX_RESULT_BUSY : TX_RESULT_CAD_TIMEOUT;
        daemon_radio_stats_record_tx_result(&ctrl->stats, busy_result);
        daemon_debug_ctx(tx->log_ctx, "Kanal belegt");
        printf("[%s] Kanal belegt, Paket verworfen\n", tag);
        printf("[%s] DATA-TX abgebrochen: %s\n", tag,
               tx_result_name(busy_result));
        return DAEMON_TX_OUTCOME_CHANNEL_BUSY;
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
        return daemon_tx_outcome_from_tx_result(result);
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
