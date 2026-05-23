#include "../framed_data.h"

#include <stdio.h>
#include <string.h>

/* --- Framed DATA protocol helper tests ---------------------------------- */

static int g_ok = 0;
static int g_fail = 0;

static void expect_int(const char *name, int actual, int expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %d, got %d\n", name, expected, actual);
    }
}

static void expect_size(const char *name, size_t actual, size_t expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %zu, got %zu\n", name, expected, actual);
    }
}

static void test_frame_type_contract(void)
{
    expect_int("RX type known", framed_data_type_known(FRAMED_DATA_TYPE_RX_PACKET), 1);
    expect_int("TX type known", framed_data_type_known(FRAMED_DATA_TYPE_TX_PACKET), 1);
    expect_int("ERROR type known", framed_data_type_known(FRAMED_DATA_TYPE_ERROR), 1);
    expect_int("unknown type rejected", framed_data_type_known(0x7F), 0);
}

static void test_rf_payload_limit(void)
{
    expect_int("zero RF payload valid", framed_data_rf_payload_len_valid(0), 1);
    expect_int("max RF payload valid", framed_data_rf_payload_len_valid(255), 1);
    expect_int("oversized RF payload invalid", framed_data_rf_payload_len_valid(256), 0);
}

static void test_header_encode_decode_little_endian(void)
{
    uint8_t header[FRAMED_DATA_HEADER_LEN];
    uint8_t type = 0;
    uint16_t len = 0;

    expect_int("encode RX header", framed_data_encode_header(header, sizeof(header),
                                                             FRAMED_DATA_TYPE_RX_PACKET,
                                                             5), 0);
    expect_int("encoded type", header[0], FRAMED_DATA_TYPE_RX_PACKET);
    expect_int("encoded len low", header[1], 5);
    expect_int("encoded len high", header[2], 0);

    expect_int("decode RX header", framed_data_decode_header(header, sizeof(header),
                                                             &type, &len), 0);
    expect_int("decoded type", type, FRAMED_DATA_TYPE_RX_PACKET);
    expect_int("decoded len", len, 5);
}

static void test_frame_size(void)
{
    expect_size("empty frame size", framed_data_frame_size(0), FRAMED_DATA_HEADER_LEN);
    expect_size("max RF frame size", framed_data_frame_size(255),
                FRAMED_DATA_HEADER_LEN + 255);
}

static void test_reject_invalid_headers(void)
{
    uint8_t header[FRAMED_DATA_HEADER_LEN];
    uint8_t type = 0;
    uint16_t len = 0;

    expect_int("encode unknown type fails",
               framed_data_encode_header(header, sizeof(header), 0x7F, 1), -1);
    expect_int("encode TX oversized fails",
               framed_data_encode_header(header, sizeof(header),
                                         FRAMED_DATA_TYPE_TX_PACKET, 256), -1);
    expect_int("encode RX oversized fails",
               framed_data_encode_header(header, sizeof(header),
                                         FRAMED_DATA_TYPE_RX_PACKET, 256), -1);
    expect_int("encode ERROR larger text ok",
               framed_data_encode_header(header, sizeof(header),
                                         FRAMED_DATA_TYPE_ERROR, 512), 0);
    expect_int("decode short header fails",
               framed_data_decode_header(header, FRAMED_DATA_HEADER_LEN - 1,
                                         &type, &len), -1);

    header[0] = FRAMED_DATA_TYPE_TX_PACKET;
    header[1] = 0x00;
    header[2] = 0x01;
    expect_int("decode TX oversized fails",
               framed_data_decode_header(header, sizeof(header), &type, &len), -1);
}


static void test_frame_encode_builder(void)
{
    const uint8_t payload[] = { 0xAA, 0xBB, 0xCC };
    uint8_t frame[FRAMED_DATA_HEADER_LEN + sizeof(payload)];
    uint8_t small[FRAMED_DATA_HEADER_LEN + sizeof(payload) - 1];

    expect_int("encode RX frame ok",
               framed_data_encode_frame(frame, sizeof(frame),
                                        FRAMED_DATA_TYPE_RX_PACKET,
                                        payload,
                                        sizeof(payload)), 0);
    expect_int("frame type", frame[0], FRAMED_DATA_TYPE_RX_PACKET);
    expect_int("frame len low", frame[1], sizeof(payload));
    expect_int("frame len high", frame[2], 0);
    expect_int("frame payload[0]", frame[3], 0xAA);
    expect_int("frame payload[2]", frame[5], 0xCC);

    expect_int("encode frame small buffer fails",
               framed_data_encode_frame(small, sizeof(small),
                                        FRAMED_DATA_TYPE_RX_PACKET,
                                        payload,
                                        sizeof(payload)), -1);
    expect_int("encode frame null payload fails",
               framed_data_encode_frame(frame, sizeof(frame),
                                        FRAMED_DATA_TYPE_RX_PACKET,
                                        NULL,
                                        sizeof(payload)), -1);
}

static void test_null_arguments(void)
{
    uint8_t header[FRAMED_DATA_HEADER_LEN];
    uint8_t type = 0;
    uint16_t len = 0;

    expect_int("encode null header fails",
               framed_data_encode_header(NULL, sizeof(header),
                                         FRAMED_DATA_TYPE_TX_PACKET, 1), -1);
    expect_int("encode short buffer fails",
               framed_data_encode_header(header, FRAMED_DATA_HEADER_LEN - 1,
                                         FRAMED_DATA_TYPE_TX_PACKET, 1), -1);
    expect_int("decode null header fails",
               framed_data_decode_header(NULL, sizeof(header), &type, &len), -1);
    expect_int("decode null type fails",
               framed_data_decode_header(header, sizeof(header), NULL, &len), -1);
    expect_int("decode null len fails",
               framed_data_decode_header(header, sizeof(header), &type, NULL), -1);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bin") == 0) {
            if (i + 1 >= argc) {
                printf("Usage: %s [--bin ignored]\n", argv[0]);
                return 2;
            }
            i++;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--bin ignored]\n", argv[0]);
            return 0;
        } else {
            printf("Usage: %s [--bin ignored]\n", argv[0]);
            return 2;
        }
    }

    test_frame_type_contract();
    test_rf_payload_limit();
    test_header_encode_decode_little_endian();
    test_frame_size();
    test_reject_invalid_headers();
    test_frame_encode_builder();
    test_null_arguments();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
