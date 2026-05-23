#include "framed_data.h"

#include <string.h>

/* --- Framed DATA socket protocol ---------------------------------------- */

int framed_data_type_known(uint8_t frame_type)
{
    return frame_type == FRAMED_DATA_TYPE_RX_PACKET ||
           frame_type == FRAMED_DATA_TYPE_TX_PACKET ||
           frame_type == FRAMED_DATA_TYPE_ERROR;
}

int framed_data_rf_payload_len_valid(size_t payload_len)
{
    return payload_len <= FRAMED_DATA_MAX_RF_PAYLOAD;
}

size_t framed_data_frame_size(uint16_t payload_len)
{
    return FRAMED_DATA_HEADER_LEN + (size_t)payload_len;
}

static int framed_data_type_payload_len_valid(uint8_t frame_type,
                                              uint16_t payload_len)
{
    if (!framed_data_type_known(frame_type))
        return 0;

    if (frame_type == FRAMED_DATA_TYPE_RX_PACKET ||
        frame_type == FRAMED_DATA_TYPE_TX_PACKET)
        return framed_data_rf_payload_len_valid(payload_len);

    return 1;
}

int framed_data_encode_header(uint8_t *header,
                              size_t header_len,
                              uint8_t frame_type,
                              uint16_t payload_len)
{
    if (!header || header_len < FRAMED_DATA_HEADER_LEN)
        return -1;

    if (!framed_data_type_payload_len_valid(frame_type, payload_len))
        return -1;

    header[0] = frame_type;
    header[1] = (uint8_t)(payload_len & 0xFF);
    header[2] = (uint8_t)((payload_len >> 8) & 0xFF);

    return 0;
}

int framed_data_decode_header(const uint8_t *header,
                              size_t header_len,
                              uint8_t *frame_type,
                              uint16_t *payload_len)
{
    uint8_t type;
    uint16_t len;

    if (!header || !frame_type || !payload_len)
        return -1;

    if (header_len < FRAMED_DATA_HEADER_LEN)
        return -1;

    type = header[0];
    len = (uint16_t)header[1] | ((uint16_t)header[2] << 8);

    if (!framed_data_type_payload_len_valid(type, len))
        return -1;

    *frame_type = type;
    *payload_len = len;

    return 0;
}


int framed_data_encode_frame(uint8_t *frame,
                             size_t frame_len,
                             uint8_t frame_type,
                             const uint8_t *payload,
                             uint16_t payload_len)
{
    if (!frame)
        return -1;

    if (payload_len > 0 && !payload)
        return -1;

    if (frame_len < framed_data_frame_size(payload_len))
        return -1;

    if (framed_data_encode_header(frame,
                                  frame_len,
                                  frame_type,
                                  payload_len) != 0)
        return -1;

    if (payload_len > 0)
        memcpy(frame + FRAMED_DATA_HEADER_LEN, payload, payload_len);

    return 0;
}
