#include "../framed_data_tx.h"

#include <stdio.h>
#include <string.h>

/*
 * M0 characterization guard for the CAD/TX signaling rework.
 *
 * Desired future behavior (M1): a recoverable RF/TX execution failure must be
 * reported to the framed client and must not make the framed TX parser return a
 * fatal error to the socket runtime.
 *
 * Current behavior: the TX handler failure is propagated as -1. Keep that as an
 * expected failure in M0; M1 will flip this test to a normal passing assertion.
 */

static int g_ok = 0;
static int g_fail = 0;
static int g_xfail = 0;
static int g_xpass = 0;

static int failing_rf_tx(uint8_t *payload, size_t len, void *ctx)
{
    (void)payload;
    (void)len;
    (void)ctx;
    return -1;
}

static void record_error(const char *msg, void *ctx)
{
    int *count = (int *)ctx;
    (void)msg;
    (*count)++;
}

static void make_tx_frame(uint8_t *frame, const uint8_t *payload, uint16_t len)
{
    framed_data_encode_header(frame,
                              FRAMED_DATA_HEADER_LEN,
                              FRAMED_DATA_TYPE_TX_PACKET,
                              len);
    if (len > 0)
        memcpy(frame + FRAMED_DATA_HEADER_LEN, payload, len);
}

static void characterize_tx_failure(void)
{
    FramedDataTxState state;
    const uint8_t payload[] = { 0x42 };
    uint8_t frame[FRAMED_DATA_HEADER_LEN + sizeof(payload)];
    int error_count = 0;
    int rc;

    framed_data_tx_state_init(&state);
    make_tx_frame(frame, payload, (uint16_t)sizeof(payload));

    rc = framed_data_tx_feed_state(&state,
                                   frame,
                                   sizeof(frame),
                                   failing_rf_tx,
                                   NULL,
                                   record_error,
                                   &error_count);

    if (rc == -1 && error_count == 0) {
        g_xfail++;
        printf("[XFAIL] RF TX failure currently propagates fatal parser error\n");
        return;
    }

    if (rc == 0 && error_count == 1) {
        g_xpass++;
        printf("[XPASS] RF TX failure already keeps framed parser alive\n");
        return;
    }

    g_fail++;
    printf("[FAIL] unexpected TX failure behavior: rc=%d error_count=%d\n",
           rc,
           error_count);
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

    characterize_tx_failure();

    printf("\nSummary: ok=%d fail=%d skip=0 xfail=%d xpass=%d\n",
           g_ok,
           g_fail,
           g_xfail,
           g_xpass);

    return (g_fail || g_xpass) ? 1 : 0;
}
