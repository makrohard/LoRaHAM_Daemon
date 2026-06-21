#ifndef LORAHAM_SOCK_H
#define LORAHAM_SOCK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "loraham_kiss_tnc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORAHAM_APRS_HDR0 0x3c
#define LORAHAM_APRS_HDR1 0xff
#define LORAHAM_APRS_HDR2 0x01
#define LHKT_LORAHAM_RX_PENDING 1024
#define LHKT_FRAMED_MAX_PAYLOAD 1024

#define LORAHAM_FRAME_RX_PACKET 0x01
#define LORAHAM_FRAME_TX_PACKET 0x02
#define LORAHAM_FRAME_ERROR     0x03
#define LORAHAM_FRAME_TX_RESULT 0x04

#define LORAHAM_TX_RESULT_LEN 4

#define LORAHAM_TX_STATUS_OK              0
#define LORAHAM_TX_STATUS_BUSY            1
#define LORAHAM_TX_STATUS_CHANNEL_BUSY    2
#define LORAHAM_TX_STATUS_RADIO_NOT_READY 3
#define LORAHAM_TX_STATUS_RADIO_ERROR     4
#define LORAHAM_TX_STATUS_INVALID_PACKET  5
#define LORAHAM_TX_STATUS_INVALID_BAND    6

#define LORAHAM_TX_RESULT_FLAG_MANAGED     0x01
#define LORAHAM_TX_RESULT_FLAG_DEFERRED    0x02
#define LORAHAM_TX_RESULT_FLAG_CAD_TIMEOUT 0x04


typedef struct {
    uint8_t buf[LHKT_TNC2_MAX_LINE];
    size_t len;
    int seen_header;
    size_t hdr_len;
    uint8_t pending[LHKT_LORAHAM_RX_PENDING];
    size_t pending_len;
} loraham_rx_state_t;

typedef struct {
    uint8_t type;
    uint8_t payload[LHKT_FRAMED_MAX_PAYLOAD];
    size_t payload_len;
} loraham_frame_t;

typedef struct {
    uint8_t header[3];
    size_t header_len;
    uint8_t type;
    uint16_t payload_len;
    uint16_t payload_pos;
    uint8_t payload[LHKT_FRAMED_MAX_PAYLOAD];
} loraham_framed_rx_state_t;

typedef struct {
    uint8_t status;
    uint8_t flags;
    uint16_t seq;
} loraham_tx_result_t;


void loraham_rx_state_init(loraham_rx_state_t *state);

int loraham_sock_connect(const char *path);

ssize_t loraham_sock_read(int fd,
                          uint8_t *buf,
                          size_t buf_size);

ssize_t loraham_sock_write(int fd,
                           const uint8_t *buf,
                           size_t len);

void loraham_framed_rx_state_init(loraham_framed_rx_state_t *state);

int loraham_framed_decode_byte(loraham_framed_rx_state_t *state,
                               uint8_t byte,
                               loraham_frame_t *frame);

int loraham_tx_result_status_valid(uint8_t status);
int loraham_decode_tx_result(const loraham_frame_t *frame,
                             loraham_tx_result_t *out);


int loraham_send_framed_tx_packet(int fd,
                                  const uint8_t *payload,
                                  size_t payload_len);

int loraham_build_aprs_packet(const char *tnc2,
                              uint8_t *out,
                              size_t out_size,
                              size_t *out_len);

int loraham_extract_tnc2(loraham_rx_state_t *state,
                         const uint8_t *data,
                         size_t data_len,
                         char *tnc2_out,
                         size_t tnc2_size,
                         size_t *tnc2_len);

int loraham_send_config(int conf_fd,
                        const lhkt_config_t *cfg);

int loraham_send_config_freq(int conf_fd,
                             const lhkt_config_t *cfg,
                             double freq);

int loraham_send_txresult_enable(int conf_fd);


#ifdef __cplusplus
}
#endif

#endif
