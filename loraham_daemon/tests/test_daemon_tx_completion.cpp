#include "../daemon_tx_completion.h"

#include <stdio.h>
#include <string.h>

/* --- TX completion frame bridge tests ----------------------------------- */

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

static DaemonTxJob make_job(uint16_t seq, uint8_t flags)
{
    DaemonTxJob job;
    uint8_t payload[] = { 0x12 };

    daemon_tx_job_init(&job, 433, 0, seq);
    daemon_tx_job_set_payload(&job, payload, sizeof(payload));
    job.flags = flags;

    return job;
}

static void test_frame_len(void)
{
    expect_size("completion frame len",
                daemon_tx_completion_frame_len(),
                FRAMED_DATA_HEADER_LEN + FRAMED_DATA_TX_RESULT_PAYLOAD_LEN);
}

static void test_encode_ok_frame(void)
{
    DaemonTxJob job = make_job(0x1234, FRAMED_DATA_TX_RESULT_FLAG_DEFERRED);
    DaemonTxJobResult result;
    uint8_t frame[FRAMED_DATA_HEADER_LEN + FRAMED_DATA_TX_RESULT_PAYLOAD_LEN];

    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_OK);

    expect_int("completion ok encode",
               daemon_tx_completion_encode_frame(frame, sizeof(frame), &result),
               0);
    expect_int("completion type", frame[0], FRAMED_DATA_TYPE_TX_RESULT);
    expect_int("completion len lo", frame[1], FRAMED_DATA_TX_RESULT_PAYLOAD_LEN);
    expect_int("completion len hi", frame[2], 0);
    expect_int("completion status ok", frame[3], FRAMED_DATA_TX_STATUS_OK);
    expect_int("completion flags", frame[4], FRAMED_DATA_TX_RESULT_FLAG_DEFERRED);
    expect_int("completion seq lo", frame[5], 0x34);
    expect_int("completion seq hi", frame[6], 0x12);
}

static void test_encode_error_frame(void)
{
    DaemonTxJob job = make_job(7, FRAMED_DATA_TX_RESULT_FLAG_MANAGED);
    DaemonTxJobResult result;
    uint8_t frame[FRAMED_DATA_HEADER_LEN + FRAMED_DATA_TX_RESULT_PAYLOAD_LEN];

    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_RADIO_ERROR);

    expect_int("completion error encode",
               daemon_tx_completion_encode_frame(frame, sizeof(frame), &result),
               0);
    expect_int("completion status radio error",
               frame[3],
               FRAMED_DATA_TX_STATUS_RADIO_ERROR);
    expect_int("completion error seq", frame[5], 7);
}

static void test_encode_rejects_bad_args(void)
{
    DaemonTxJob job = make_job(1, 0);
    DaemonTxJobResult result;
    uint8_t frame[FRAMED_DATA_HEADER_LEN + FRAMED_DATA_TX_RESULT_PAYLOAD_LEN];

    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_OK);

    expect_int("completion null result",
               daemon_tx_completion_encode_frame(frame, sizeof(frame), NULL),
               -1);
    expect_int("completion short frame",
               daemon_tx_completion_encode_frame(frame, sizeof(frame) - 1, &result),
               -1);
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

    test_frame_len();
    test_encode_ok_frame();
    test_encode_error_frame();
    test_encode_rejects_bad_args();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
