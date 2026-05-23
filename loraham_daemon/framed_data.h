#ifndef LORAHAM_FRAMED_DATA_H
#define LORAHAM_FRAMED_DATA_H

#include <stddef.h>
#include <stdint.h>

/* --- Framed DATA socket protocol ---------------------------------------- */

#define FRAMED_DATA_HEADER_LEN 3
#define FRAMED_DATA_MAX_RF_PAYLOAD 255

#define FRAMED_DATA_TYPE_RX_PACKET 0x01
#define FRAMED_DATA_TYPE_TX_PACKET 0x02
#define FRAMED_DATA_TYPE_ERROR     0x03

int framed_data_type_known(uint8_t frame_type);
int framed_data_rf_payload_len_valid(size_t payload_len);
size_t framed_data_frame_size(uint16_t payload_len);

int framed_data_encode_header(uint8_t *header,
                              size_t header_len,
                              uint8_t frame_type,
                              uint16_t payload_len);

int framed_data_decode_header(const uint8_t *header,
                              size_t header_len,
                              uint8_t *frame_type,
                              uint16_t *payload_len);

#endif
