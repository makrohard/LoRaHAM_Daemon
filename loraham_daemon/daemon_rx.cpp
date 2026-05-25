#include "daemon_rx.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <RadioLib.h>

#include "client_slot.h"
#include "daemon_log.h"
#include "daemon_protocol.h"
#include "daemon_radio_runtime.h"
#include "daemon_stats.h"
#include "framed_data.h"
#include "radio_channel.h"
#include "radio_controller.h"
#include "rf_packet.h"

/* --- External daemon channel state -------------------------------------- */

extern RadioChannelIo channel_433;
extern RadioChannelIo channel_868;

/* --- RX drop logging ----------------------------------------------------- */
// Rate-limited counters for invalid RadioLib RX reads.
#define RX_DROP_LOG_INITIAL 5
#define RX_DROP_LOG_INTERVAL 100


/* --- RX log context ------------------------------------------------------ */
template<typename RadioT>
static const char *daemon_rx_log_ctx(RadioController<RadioT> *ctrl)
{
    return (ctrl && ctrl->band == RADIO_BAND_433) ? "RX433" : "RX868";
}

/* --- RX special cases ---------------------------------------------------- */
template<typename RadioT>
static void daemon_discard_rx_during_tx(RadioController<RadioT> *ctrl)
{
    ctrl->received.store(false);
    ctrl->radio->clearIrq(0xFFFFFFFF);
    daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "RX während TX verworfen");
    printf("[%s] RX während TX - verwerfe Paket\n",
           radio_controller_tag(ctrl));
}

/* --- RX output ----------------------------------------------------------- */
static void daemon_debug_hex_bytes(const char *ctx, const uint8_t *buf, int len)
{
    char msg[512];
    size_t pos = 0;

    pos += snprintf(msg + pos, sizeof(msg) - pos, "HEX:");
    for (int i = 0; i < len; i++) {
        if (pos + 4 >= sizeof(msg)) {
            snprintf(msg + pos, sizeof(msg) - pos, " ...");
            break;
        }

        pos += snprintf(msg + pos, sizeof(msg) - pos, " %02X", buf[i]);
    }

    daemon_debug_ctx(ctx, "%s", msg);
}

static void daemon_print_ascii_bytes(const uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++) {
        if (buf[i] >= 32 && buf[i] <= 126)
            printf("%c", buf[i]);
        else
            printf(".");
    }
}

/* --- RX presentation metadata ------------------------------------------- */
template<typename RadioT>
static const char *daemon_controller_color(RadioController<RadioT> *ctrl)
{
    if (ctrl->band == RADIO_BAND_433)
        return "93m";

    return "32m";
}

static void daemon_print_raw_rx_packet(const char *rx_ctx,
                                       const char *band,
                                       const char *color,
                                       const char *suffix,
                                       uint8_t *buf,
                                       int len,
                                       float rssi)
{
    daemon_debug_hex_bytes(rx_ctx, buf, len);

    printf("[\e[%s%s%s\e[0m] %d Bytes ASCII: ",
           color, band, suffix ? suffix : "", len);
    daemon_print_ascii_bytes(buf, len);
    printf(" RSSI: %.2f dBm\n", rssi);
}

static void daemon_print_lora_packet(const char *rx_ctx,
                                     const char *band,
                                     const char *color,
                                     uint8_t *buf,
                                     int len,
                                     float rssi)
{
    if (!rf_packet_lora_header_available((size_t)len)) {
        daemon_debug_ctx(rx_ctx,
                         "LoRa short packet %d Byte, header skipped",
                         len);
        daemon_print_raw_rx_packet(rx_ctx, band, color, "-SHORT",
                                   buf, len, rssi);
        return;
    }

    uint32_t toNode      = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
    uint32_t fromNode    = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
    uint32_t uniqueID    = buf[8] | (buf[9] << 8) | (buf[10] << 16) | (buf[11] << 24);

    uint8_t hdrFlags     = buf[12];
    uint8_t chHash       = buf[13];
    uint8_t nextHop      = buf[14];
    uint8_t rlyNodes     = buf[15];

    daemon_debug_ctx(rx_ctx,
                     "LoRa %d Byte from %08X to %08X ID:%08X Flag:%02X Hash:%02X Hop:%02X Node:%02X RSSI: %.2f dBm",
                     len, fromNode, toNode, uniqueID,
                     hdrFlags, chHash, nextHop, rlyNodes, rssi);
    daemon_debug_hex_bytes(rx_ctx, buf, len);

    printf("[\e[%s%s\e[0m] %d Bytes ASCII: ", color, band, len);
    daemon_print_ascii_bytes(buf, len);
    printf(" RSSI: %.2f dBm\n", rssi);
}

static void daemon_print_fsk_packet(const char *rx_ctx,
                                    const char *band,
                                    const char *color,
                                    uint8_t *buf,
                                    int len,
                                    float rssi)
{
    daemon_debug_hex_bytes(rx_ctx, buf, len);

    printf("[\e[%s%s-FSK\e[0m] %d Bytes ASCII: ", color, band, len);
    daemon_print_ascii_bytes(buf, len);
    printf(" RSSI: %.2f dBm\n", rssi);
}

/* --- RX forwarding ------------------------------------------------------- */
static void daemon_broadcast_rx_data(RadioChannelIo *io, uint8_t *buf, int len)
{
    uint8_t frame[FRAMED_DATA_HEADER_LEN + FRAMED_DATA_MAX_RF_PAYLOAD];

    if (len <= 0)
        return;

    client_slot_broadcast_bytes_queued(io->data_slots, MAX_CLIENTS, buf, len);

    if ((size_t)len > FRAMED_DATA_MAX_RF_PAYLOAD) {
        daemon_debug_ctx("RXF", "RX frame too large: %d", len);
        return;
    }

    if (framed_data_encode_frame(frame,
                                 sizeof(frame),
                                 FRAMED_DATA_TYPE_RX_PACKET,
                                 buf,
                                 (uint16_t)len) != 0) {
        daemon_debug_ctx("RXF", "RX frame encode failed");
        return;
    }

    client_slot_broadcast_bytes_queued(io->framed_data_slots,
                                       MAX_CLIENTS,
                                       frame,
                                       framed_data_frame_size((uint16_t)len));
}

/* --- RX IRQ/FIFO sequence ------------------------------------------------ */
template<typename RadioT>
static void daemon_restart_receive_after_empty_rx(RadioController<RadioT> *ctrl)
{
    daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "Leer-IRQ, RX neu starten");
    ctrl->radio->clearIrq(0xFFFFFFFF);
    ctrl->radio->startReceive();
}

template<typename RadioT>
static void daemon_finish_rx_packet(RadioController<RadioT> *ctrl,
                                    uint8_t *buf,
                                    size_t buf_len)
{
    if (ctrl->band == RADIO_BAND_868)
        memset(buf, 0, buf_len);

    daemon_radio_runtime_led(ctrl, 0);
    ctrl->radio->startReceive();
    daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "RX bereit");
}

template<typename RadioT>
static void daemon_prepare_rx_packet(RadioController<RadioT> *ctrl,
                                     uint8_t *buf,
                                     size_t buf_len)
{
    ctrl->received.store(false);
    memset(buf, 0, buf_len);

    // In FSK mode, do not clear IRQs before reading the FIFO.
    // Writing IRQ flags here can set FifoOverrun and clear the FIFO.
    if (ctrl->mode == RADIO_MODE_LORA) {
        daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "IRQ vor Read löschen");
        ctrl->radio->clearIrq(0xFFFFFFFF);
    }
}

template<typename RadioT>
static int daemon_rx_packet_length(RadioController<RadioT> *ctrl)
{
    int len = ctrl->radio->getPacketLength();

    daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "Länge %d", len);
    return len;
}

template<typename RadioT>
static void daemon_clear_irq_after_rx_read(RadioController<RadioT> *ctrl)
{
    if (ctrl->mode == RADIO_MODE_FSK) {
        daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "IRQ nach Read löschen");
        ctrl->radio->clearIrq(0xFFFFFFFF);
    }
}

template<typename RadioT>
static void daemon_print_rx_packet(RadioController<RadioT> *ctrl,
                                   uint8_t *buf,
                                   int len)
{
    const char *tag = radio_controller_tag(ctrl);
    const char *color = daemon_controller_color(ctrl);
    float rssi = radio_controller_packet_rssi(ctrl);

    if (ctrl->mode == RADIO_MODE_LORA)
        daemon_print_lora_packet(daemon_rx_log_ctx(ctrl), tag, color, buf, len, rssi);
    else
        daemon_print_fsk_packet(daemon_rx_log_ctx(ctrl), tag, color, buf, len, rssi);
}

/* --- RX radio accessors -------------------------------------------------- */
template<typename RadioT>
static int16_t daemon_read_rx_data(RadioController<RadioT> *ctrl,
                                   uint8_t *buf,
                                   size_t buf_len)
{
    return ctrl->radio->readData(buf, buf_len);
}

/* --- RX read validation -------------------------------------------------- */
static bool daemon_should_log_rx_drop(unsigned long drops)
{
    return drops <= RX_DROP_LOG_INITIAL ||
           (RX_DROP_LOG_INTERVAL > 0 && drops % RX_DROP_LOG_INTERVAL == 0);
}

template<typename RadioT>
static void daemon_record_rx_drop(RadioController<RadioT> *ctrl, int16_t state)
{
    daemon_radio_stats_record_rx_drop(&ctrl->stats);
    daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "Drop %lu Status %d",
                     ctrl->stats.rx_drops, state);

    if (daemon_should_log_rx_drop(ctrl->stats.rx_drops)) {
        printf("[%d] RX read error: %d, packet dropped, drops=%lu\n",
               radio_controller_band_number(ctrl), state, ctrl->stats.rx_drops);
        fflush(stdout);
    }
}

template<typename RadioT>
static bool daemon_rx_read_ok(RadioController<RadioT> *ctrl, int16_t state)
{
    if (state == RADIOLIB_ERR_NONE)
        return true;

    daemon_record_rx_drop(ctrl, state);
    return false;
}

template<typename RadioT>
static void daemon_record_rx_invalid_packet(RadioController<RadioT> *ctrl,
                                            const char *reason,
                                            int len)
{
    daemon_radio_stats_record_rx_drop(&ctrl->stats);
    daemon_debug_ctx(daemon_rx_log_ctx(ctrl),
                     "Drop %lu invalid packet: %s (%d Byte)",
                     ctrl->stats.rx_drops,
                     reason ? reason : "invalid",
                     len);

    if (daemon_should_log_rx_drop(ctrl->stats.rx_drops)) {
        printf("[%d] RX invalid packet: %s (%d bytes), packet dropped, drops=%lu\n",
               radio_controller_band_number(ctrl),
               reason ? reason : "invalid",
               len,
               ctrl->stats.rx_drops);
        fflush(stdout);
    }
}

template<typename RadioT>
static bool daemon_rx_length_ok(RadioController<RadioT> *ctrl,
                                int len,
                                size_t buf_len)
{
    if (len <= 0)
        return false;

    if ((size_t)len > buf_len || (size_t)len > RF_PACKET_MAX_PAYLOAD_LEN) {
        daemon_record_rx_invalid_packet(ctrl, "packet too long", len);
        return false;
    }

    return true;
}

template<typename RadioT>
static bool daemon_rx_packet_ok(RadioController<RadioT> *ctrl,
                                uint8_t *buf,
                                int len)
{
    RfPacketValidation packet_state =
        rf_packet_validate(buf, (size_t)len);

    if (packet_state != RF_PACKET_VALID) {
        daemon_record_rx_invalid_packet(
            ctrl,
            rf_packet_validation_message(packet_state),
            len);
        return false;
    }

    return true;
}

template<typename RadioT>
static void daemon_drop_invalid_rx_packet(RadioController<RadioT> *ctrl)
{
    ctrl->received.store(false);
    ctrl->radio->clearIrq(0xFFFFFFFF);
    daemon_radio_runtime_led(ctrl, 0);
    ctrl->radio->startReceive();
    daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "RX bereit nach Drop");
}


/* --- RX band flow -------------------------------------------------------- */
template<typename RadioT>
static void daemon_process_radio_band(RadioController<RadioT> *ctrl,
                                      RadioChannelIo *io,
                                      uint8_t (&rx_buf)[buf_SIZE])
{
    if (!radio_controller_ready(ctrl) || !ctrl->radio)
        return;

    // Shared RX flow for both bands.
    if (!ctrl->received.load())
        return;

    if (ctrl->tx_busy.load()) {
        daemon_discard_rx_during_tx(ctrl);
        return;
    }

    daemon_prepare_rx_packet(ctrl, rx_buf, sizeof(rx_buf));

    int len = daemon_rx_packet_length(ctrl);
    // Spurious IRQ: no packet is available.
    if (len <= 0) {
        // FSK IRQ clear is safe here because the FIFO is empty.
        daemon_restart_receive_after_empty_rx(ctrl);
        return;
    }

    if (!daemon_rx_length_ok(ctrl, len, sizeof(rx_buf))) {
        daemon_drop_invalid_rx_packet(ctrl);
        return;
    }

    daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "readData()");
    int16_t read_state = daemon_read_rx_data(ctrl, rx_buf, sizeof(rx_buf)); // 5 ms timeout

    // In FSK mode, clear IRQs after readData() emptied the FIFO.
    daemon_clear_irq_after_rx_read(ctrl);

    if (!daemon_rx_read_ok(ctrl, read_state)) {
        daemon_finish_rx_packet(ctrl, rx_buf, sizeof(rx_buf));
        return;
    }

    daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "Read OK");

    if (!daemon_rx_packet_ok(ctrl, rx_buf, len)) {
        daemon_finish_rx_packet(ctrl, rx_buf, sizeof(rx_buf));
        return;
    }

    daemon_radio_stats_record_rx(&ctrl->stats, (size_t)len);
    daemon_print_rx_packet(ctrl, rx_buf, len);
    daemon_broadcast_rx_data(io, rx_buf, len);
    daemon_debug_ctx(daemon_rx_log_ctx(ctrl), "Broadcast %d Byte", len);

    daemon_finish_rx_packet(ctrl, rx_buf, sizeof(rx_buf));
}


/* --- RX band entry points ------------------------------------------------ */
void daemon_process_radio_433(uint8_t (&rx_buf_433)[buf_SIZE])
{
    // Band-specific RX entry point.
    daemon_process_radio_band(&radio_controller_433, &channel_433, rx_buf_433);
}

void daemon_process_radio_868(uint8_t (&rx_buf_868)[buf_SIZE])
{
    // Band-specific RX entry point.
    daemon_process_radio_band(&radio_controller_868, &channel_868, rx_buf_868);
}
