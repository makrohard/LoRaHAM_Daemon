#include "daemon_tx.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <RadioLib.h>

#include "client_slot.h"
#include "daemon_log.h"
#include "daemon_protocol.h"
#include "daemon_radio_runtime.h"
#include "radio_channel.h"
#include "radio_controller.h"
#include "radio_health.h"
#include "rf_packet.h"

/* --- External daemon socket state --------------------------------------- */

extern RadioChannelIo channel_433;
extern RadioChannelIo channel_868;

/* --- LoRa TX ------------------------------------------------------------- */
static bool lora_send_valid_band(int band)
{
    return band == 433 || band == 868;
}

static RadioHealth daemon_radio_health(int band)
{
    if (!lora_send_valid_band(band))
        return RADIO_HEALTH_FAILED;

    if (band == 433)
        return radio_controller_health(&radio_controller_433);

    return radio_controller_health(&radio_controller_868);
}

static bool daemon_radio_ready(int band)
{
    if (!lora_send_valid_band(band))
        return false;

    if (band == 433)
        return radio_controller_ready(&radio_controller_433);

    return radio_controller_ready(&radio_controller_868);
}

static const char *lora_tx_log_ctx(int band)
{
    return band == 433 ? "TX433" : "TX868";
}

/* --- TX status ----------------------------------------------------------- */
// Broadcast local TX state on the matching CONF socket.
static void daemon_broadcast_tx_status(RadioBand_t band, bool busy)
{
    RadioChannelIo *channel = (band == RADIO_BAND_433) ? &channel_433 : &channel_868;

    client_slot_broadcast_queued(channel->conf_slots,
                                 MAX_CLIENTS,
                                 busy ? "TX=1\n" : "TX=0\n");
}

template<typename RadioT>
static bool lora_send_acquire_controller_tx(RadioController<RadioT> *ctrl)
{
    if (!ctrl)
        return false;

    return !ctrl->tx_busy.exchange(true);
}

template<typename RadioT>
static void lora_send_release_controller_tx(RadioController<RadioT> *ctrl)
{
    ctrl->tx_busy = false;
    daemon_broadcast_tx_status(ctrl->band, false);
}

static void lora_print_tx_preview(const char *ctx,
                                  const uint8_t *buf,
                                  size_t len)
{
    char msg[512];
    size_t pos = 0;

    pos += snprintf(msg + pos, sizeof(msg) - pos, "%zu Byte: ", len);

    for(size_t i = 0; i < len && pos < sizeof(msg); i++)
        pos += snprintf(msg + pos, sizeof(msg) - pos, "%c",
                        buf[i] >= 32 && buf[i] <= 126 ? buf[i] : '.');

    printf("[%s] %s\n", ctx, msg);
    fflush(stdout);
}

static void lora_debug_tx_preview(const char *ctx,
                                  const uint8_t *buf,
                                  size_t len)
{
    size_t preview_len = rf_packet_preview_len(len);
    char msg[512];
    size_t pos = 0;

    pos += snprintf(msg + pos, sizeof(msg) - pos, "HEX:");

    for(size_t i = 0; i < preview_len && pos < sizeof(msg); i++)
        pos += snprintf(msg + pos, sizeof(msg) - pos, " %02X", buf[i]);

    if (preview_len < len && pos < sizeof(msg))
        snprintf(msg + pos, sizeof(msg) - pos, " ...");

    daemon_debug_ctx(ctx, "%s", msg);
}

static void lora_debug_tx_first_bytes(const char *ctx,
                                      const uint8_t *buf,
                                      size_t len)
{
    size_t preview_len = len < 7 ? len : 7;
    char msg[160];
    size_t pos = 0;

    pos += snprintf(msg + pos, sizeof(msg) - pos, "Sende jetzt %zu Byte", len);
    if (preview_len > 0) {
        pos += snprintf(msg + pos, sizeof(msg) - pos, " (");
        for (size_t i = 0; i < preview_len && pos < sizeof(msg); i++) {
            pos += snprintf(msg + pos, sizeof(msg) - pos,
                            "%s0x%02X", i > 0 ? " " : "", buf[i]);
        }
        snprintf(msg + pos, sizeof(msg) - pos, ")");
    }

    daemon_debug_ctx(ctx, "%s", msg);
}

template<typename RadioT>
static void lora_send_prepare_controller_tx(RadioController<RadioT> *ctrl)
{
    // TX ownership was acquired before payload copy/logging.
    daemon_broadcast_tx_status(ctrl->band, true);

    // Clear RX state before switching the radio to TX.
    ctrl->received = false;

    // FSK uses a different receive path.
    if (ctrl->mode == RADIO_MODE_LORA)
        ctrl->radio->clearPacketReceivedAction();

    // Move the radio into a clean TX state.
    ctrl->radio->standby();
    ctrl->radio->clearIrq(0xFFFFFFFF);

    // Reset the TX FIFO only for 433/LoRa.
    // FSK sleep would reset the module configuration.
    if (ctrl->band == RADIO_BAND_433 && ctrl->mode == RADIO_MODE_LORA) {
        ctrl->radio->sleep();
        usleep(10000); // 10 ms
        ctrl->radio->standby();
        usleep(10000); // 10 ms
    }
}

template<typename RadioT>
static TxResult lora_send_controller(RadioController<RadioT> *ctrl,
                                     uint8_t *buf,
                                     size_t len)
{
    int band = radio_controller_band_number(ctrl);
    const char *tag = radio_controller_tag(ctrl);
    const char *tx_ctx = lora_tx_log_ctx(band);

    if (!ctrl || !ctrl->radio || !radio_controller_ready(ctrl)) {
        printf("[SEND %d] radio not ready: %s\n",
               band, radio_health_name(radio_controller_health(ctrl)));
        fflush(stdout);
        return TX_RESULT_RADIO_NOT_READY;
    }

    RfPacketValidation packet_state = rf_packet_validate(buf, len);
    if (packet_state != RF_PACKET_VALID) {
        printf("[SEND %d] invalid TX packet: %s (%zu bytes)\n",
               band, rf_packet_validation_message(packet_state), len);
        fflush(stdout);
        return TX_RESULT_INVALID_PACKET;
    }

    if (!lora_send_acquire_controller_tx(ctrl)) {
        printf("[%s] TX BUSY - überspringen\n", tag);
        fflush(stdout);
        return TX_RESULT_BUSY;
    }

    // Copy the buffer after TX ownership is acquired.
    uint8_t send_buf[RF_PACKET_MAX_PAYLOAD_LEN];
    memcpy(send_buf, buf, len);

    lora_print_tx_preview(tx_ctx, send_buf, len);
    lora_debug_tx_preview(tx_ctx, send_buf, len);

    lora_send_prepare_controller_tx(ctrl);

    if (ctrl->band == RADIO_BAND_433) {
        // Clear IRQs once more before TX.
        ctrl->radio->clearIrq(0xFFFFFFFF);

        daemon_debug_ctx(tx_ctx, "Radio neu konfiguriert");
        lora_debug_tx_first_bytes(tx_ctx, send_buf, len);
    }

    // transmit() blocks until the packet is sent.
    int state = ctrl->radio->transmit(send_buf, len);

    if(state != RADIOLIB_ERR_NONE) {
        daemon_debug_ctx(tx_ctx, "transmit Fehler %d", state);
        if (ctrl->band == RADIO_BAND_433)
            printf("[433] transmit ERROR: %d\n", state);
        else
            printf("[868] TX ERROR: %d\n", state);
    } else {
        daemon_debug_ctx(tx_ctx, "transmit OK");
    }

    if (ctrl->band == RADIO_BAND_868)
        usleep(50000); // 50 ms guard delay

    // Restore IRQ handling and RX callback after TX.
    // transmit() can clear the callback.
    ctrl->radio->clearIrq(0xFFFFFFFF);
    ctrl->received = false;
    ctrl->radio->setPacketReceivedAction(ctrl->rx_callback);
    lora_send_release_controller_tx(ctrl);
    ctrl->radio->startReceive();

    return state == RADIOLIB_ERR_NONE
        ? TX_RESULT_OK
        : TX_RESULT_RADIO_ERROR;
}

TxResult lora_send(uint8_t *buf, size_t len, int band) {
    if (!lora_send_valid_band(band)) {
        printf("[SEND %d] invalid band\n", band);
        fflush(stdout);
        return TX_RESULT_INVALID_BAND;
    }

    if (!daemon_radio_ready(band)) {
        printf("[SEND %d] radio not ready: %s\n",
               band, radio_health_name(daemon_radio_health(band)));
        fflush(stdout);
        return TX_RESULT_RADIO_NOT_READY;
    }

    if (band == 433)
        return lora_send_controller(&radio_controller_433, buf, len);

    return lora_send_controller(&radio_controller_868, buf, len);
}
