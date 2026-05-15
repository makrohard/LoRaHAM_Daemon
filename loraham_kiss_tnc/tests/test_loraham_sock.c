#include "loraham_sock.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_build_packet(void)
{
    uint8_t out[LHKT_LORAHAM_TX_MAX];
    size_t out_len = 0;
    const char *tnc2 = "DJ0CHE-10>APRS:hello";

    assert(loraham_build_aprs_packet(tnc2, out, sizeof(out), &out_len) == LHKT_OK);

    assert(out_len == LHKT_LORAHAM_HDR_LEN + strlen(tnc2));
    assert(out[0] == LORAHAM_APRS_HDR0);
    assert(out[1] == LORAHAM_APRS_HDR1);
    assert(out[2] == LORAHAM_APRS_HDR2);
    assert(memcmp(out + LHKT_LORAHAM_HDR_LEN, tnc2, strlen(tnc2)) == 0);
}

static void test_build_packet_limit(void)
{
    char tnc2[300];
    uint8_t out[LHKT_LORAHAM_TX_MAX];
    size_t out_len = 0;
    size_t i;

    for (i = 0; i < 252; i++) {
        tnc2[i] = 'A';
    }
    tnc2[252] = '\0';

    assert(loraham_build_aprs_packet(tnc2, out, sizeof(out), &out_len) == LHKT_OK);
    assert(out_len == LHKT_LORAHAM_TX_MAX);

    tnc2[252] = 'B';
    tnc2[253] = '\0';

    assert(loraham_build_aprs_packet(tnc2, out, sizeof(out), &out_len) == LHKT_ERR_LONG);
    assert(out_len == 0);
}

static void test_extract_with_newline(void)
{
    loraham_rx_state_t state;
    char out[LHKT_TNC2_MAX_LINE];
    size_t out_len = 0;

    const uint8_t data[] = {
        0x00, 0x11,
        LORAHAM_APRS_HDR0, LORAHAM_APRS_HDR1, LORAHAM_APRS_HDR2,
        'D','J','0','C','H','E','-','1','0','>','A','P','R','S',':','h','i','\n'
    };

    loraham_rx_state_init(&state);

    assert(loraham_extract_tnc2(&state, data, sizeof(data), out, sizeof(out), &out_len) == 1);
    assert(strcmp(out, "DJ0CHE-10>APRS:hi") == 0);
    assert(out_len == strlen(out));
}

static void test_extract_split_header(void)
{
    loraham_rx_state_t state;
    char out[LHKT_TNC2_MAX_LINE];
    size_t out_len = 0;

    const uint8_t part1[] = {
        0x99, LORAHAM_APRS_HDR0
    };

    const uint8_t part2[] = {
        LORAHAM_APRS_HDR1, LORAHAM_APRS_HDR2,
        'A','>','B',':','C','\n'
    };

    loraham_rx_state_init(&state);

    assert(loraham_extract_tnc2(&state, part1, sizeof(part1), out, sizeof(out), &out_len) == 0);
    assert(out_len == 0);

    assert(loraham_extract_tnc2(&state, part2, sizeof(part2), out, sizeof(out), &out_len) == 1);
    assert(strcmp(out, "A>B:C") == 0);
}

static void test_extract_flush_without_newline(void)
{
    loraham_rx_state_t state;
    char out[LHKT_TNC2_MAX_LINE];
    size_t out_len = 0;

    const uint8_t data[] = {
        LORAHAM_APRS_HDR0, LORAHAM_APRS_HDR1, LORAHAM_APRS_HDR2,
        'D','J','0','C','H','E','-','1','0','>','A','P','R','S',':','h','i'
    };

    loraham_rx_state_init(&state);

    assert(loraham_extract_tnc2(&state, data, sizeof(data), out, sizeof(out), &out_len) == 0);
    assert(out_len == 0);

    assert(loraham_extract_tnc2(&state, NULL, 0, out, sizeof(out), &out_len) == 1);
    assert(strcmp(out, "DJ0CHE-10>APRS:hi") == 0);
}

static void test_ignore_noise_without_header(void)
{
    loraham_rx_state_t state;
    char out[LHKT_TNC2_MAX_LINE];
    size_t out_len = 0;

    const uint8_t data[] = { 'n','o','i','s','e','\n' };

    loraham_rx_state_init(&state);

    assert(loraham_extract_tnc2(&state, data, sizeof(data), out, sizeof(out), &out_len) == 0);
    assert(out_len == 0);
}

int main(void)
{
    test_build_packet();
    test_build_packet_limit();
    test_extract_with_newline();
    test_extract_split_header();
    test_extract_flush_without_newline();
    test_ignore_noise_without_header();

    puts("test_loraham_sock: OK");
    return 0;
}
