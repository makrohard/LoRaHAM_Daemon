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

#ifdef __cplusplus
}
#endif

#endif
