#include "../daemon_tx_completion.h"
#include "../daemon_framed_data_runtime.h"
#include "../daemon_tx_async_runtime.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* --- TX completion frame bridge tests ----------------------------------- */


void daemon_drain_framed_tx_completions(const char *tag,
                                        ClientSlot *slots,
                                        int max_clients);


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


static void test_framed_immediate_result_policy(void)
{
    expect_int("framed no txresult no immediate",
               daemon_framed_tx_should_emit_immediate_result(0, 0, DAEMON_TX_OUTCOME_OK),
               0);
    expect_int("framed direct success immediate",
               daemon_framed_tx_should_emit_immediate_result(1, 0, DAEMON_TX_OUTCOME_OK),
               1);
    expect_int("framed queued success final only",
               daemon_framed_tx_should_emit_immediate_result(1, 1, DAEMON_TX_OUTCOME_OK),
               0);
    expect_int("framed queued failure immediate",
               daemon_framed_tx_should_emit_immediate_result(1, 1, DAEMON_TX_OUTCOME_CHANNEL_BUSY),
               1);
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


static void test_completion_queue_push_pop(void)
{
    DaemonTxCompletionQueue queue;
    DaemonTxJob job = make_job(11, FRAMED_DATA_TX_RESULT_FLAG_MANAGED);
    DaemonTxJobResult result;
    DaemonTxJobResult out = {};

    daemon_tx_completion_queue_init(&queue);
    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_OK);

    expect_int("completion queue push",
               daemon_tx_completion_queue_push(&queue, &result),
               0);
    expect_size("completion queue pending after push",
                daemon_tx_completion_queue_pending(&queue),
                1);
    expect_int("completion queue pop",
               daemon_tx_completion_queue_pop(&queue, &out),
               0);
    expect_int("completion queue pop result", out.tx_result, TX_RESULT_OK);
    expect_int("completion queue pop seq", out.seq, 11);
    expect_size("completion queue pending after pop",
                daemon_tx_completion_queue_pending(&queue),
                0);
}

static void test_completion_queue_drops_oldest_when_full(void)
{
    DaemonTxCompletionQueue queue;
    DaemonTxJobResult out = {};

    daemon_tx_completion_queue_init(&queue);

    for (int i = 0; i < DAEMON_TX_COMPLETION_QUEUE_CAPACITY + 1; i++) {
        DaemonTxJob job = make_job((uint16_t)i, 0);
        DaemonTxJobResult result;

        daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_OK);
        daemon_tx_completion_queue_push(&queue, &result);
    }

    expect_size("completion queue full pending",
                daemon_tx_completion_queue_pending(&queue),
                DAEMON_TX_COMPLETION_QUEUE_CAPACITY);
    expect_size("completion queue dropped oldest",
                daemon_tx_completion_queue_dropped(&queue),
                1);
    expect_int("completion queue oldest removed",
               daemon_tx_completion_queue_pop(&queue, &out),
               0);
    expect_int("completion queue first remaining seq", out.seq, 1);
}



static void test_completion_delivers_to_target_slot(void)
{
    ClientSlot slots[2];
    int sv[2];
    DaemonTxJob job = make_job(0x2222, FRAMED_DATA_TX_RESULT_FLAG_DEFERRED);
    DaemonTxJobResult result;
    uint8_t frame[FRAMED_DATA_HEADER_LEN + FRAMED_DATA_TX_RESULT_PAYLOAD_LEN];

    client_slot_init_all(slots, 2);
    expect_int("completion socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    client_slot_set_fd(&slots[1], sv[0]);

    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_OK);
    result.completion_slot = 1;
    result.completion_generation = client_slot_generation(&slots[1]);

    expect_int("completion deliver target",
               daemon_tx_completion_deliver_to_slot(slots, 2, &result),
               1);

    expect_int("completion read frame",
               (int)read(sv[1], frame, sizeof(frame)),
               (int)sizeof(frame));
    expect_int("completion delivered type", frame[0], FRAMED_DATA_TYPE_TX_RESULT);
    expect_int("completion delivered status", frame[3], FRAMED_DATA_TX_STATUS_OK);
    expect_int("completion delivered seq lo", frame[5], 0x22);
    expect_int("completion delivered seq hi", frame[6], 0x22);

    close(sv[1]);
    client_slot_close_all(slots, 2);
}

static void test_completion_ignores_reused_slot_generation(void)
{
    ClientSlot slots[1];
    int old_sv[2];
    int new_sv[2];
    DaemonTxJob job = make_job(0x4567, FRAMED_DATA_TX_RESULT_FLAG_DEFERRED);
    DaemonTxJobResult result;

    client_slot_init_all(slots, 1);
    expect_int("stale completion old socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, old_sv),
               0);
    client_slot_set_fd(&slots[0], old_sv[0]);

    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_OK);
    result.completion_slot = 0;
    result.completion_generation = client_slot_generation(&slots[0]);

    close(old_sv[1]);
    client_slot_close(&slots[0]);

    expect_int("stale completion new socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, new_sv),
               0);
    client_slot_set_fd(&slots[0], new_sv[0]);

    expect_int("stale completion ignored",
               daemon_tx_completion_deliver_to_slot(slots, 1, &result),
               DAEMON_TX_COMPLETION_DELIVERY_STALE);
    expect_size("stale completion queued bytes",
                client_output_queue_pending(&slots[0].output),
                0);

    close(new_sv[1]);
    client_slot_close_all(slots, 1);
}


static void test_completion_ignores_missing_target(void)
{
    ClientSlot slots[1];
    DaemonTxJob job = make_job(1, 0);
    DaemonTxJobResult result;

    client_slot_init_all(slots, 1);
    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_OK);
    result.completion_slot = DAEMON_TX_COMPLETION_SLOT_NONE;

    expect_int("completion ignore no target",
               daemon_tx_completion_deliver_to_slot(slots, 1, &result),
               0);

    result.completion_slot = 4;
    expect_int("completion ignore bad target",
               daemon_tx_completion_deliver_to_slot(slots, 1, &result),
               0);
}



static void test_drain_counts_stale_completion(void)
{
    ClientSlot slots[1];
    int old_sv[2];
    int new_sv[2];
    DaemonTxJob job = make_job(0x5678, FRAMED_DATA_TX_RESULT_FLAG_DEFERRED);
    DaemonTxJobResult result;

    daemon_tx_async_runtime_shutdown();
    daemon_tx_async_runtime_init();

    client_slot_init_all(slots, 1);
    expect_int("stale drain old socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, old_sv),
               0);
    client_slot_set_fd(&slots[0], old_sv[0]);

    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_OK);
    result.completion_slot = 0;
    result.completion_generation = client_slot_generation(&slots[0]);

    close(old_sv[1]);
    client_slot_close(&slots[0]);

    expect_int("stale drain new socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, new_sv),
               0);
    client_slot_set_fd(&slots[0], new_sv[0]);

    daemon_tx_completion_queue_push(
        daemon_tx_async_runtime_completion_queue(),
        &result);

    daemon_drain_framed_tx_completions("TESTF", slots, 1);

    expect_size("stale drain counter",
                daemon_tx_async_runtime_completion_stale(),
                1);
    expect_size("stale drain pending",
                daemon_tx_async_runtime_completion_pending(),
                0);
    expect_size("stale drain output empty",
                client_output_queue_pending(&slots[0].output),
                0);

    close(new_sv[1]);
    client_slot_close_all(slots, 1);
    daemon_tx_async_runtime_shutdown();
}


static void test_drain_delivers_queued_completion(void)
{
    ClientSlot slots[2];
    int sv[2];
    DaemonTxJob job = make_job(0x3456, FRAMED_DATA_TX_RESULT_FLAG_DEFERRED);
    DaemonTxJobResult result;
    uint8_t frame[FRAMED_DATA_HEADER_LEN + FRAMED_DATA_TX_RESULT_PAYLOAD_LEN];

    daemon_tx_async_runtime_shutdown();
    daemon_tx_async_runtime_init();

    client_slot_init_all(slots, 2);
    expect_int("completion drain socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, sv),
               0);

    client_slot_set_fd(&slots[0], sv[0]);

    daemon_tx_job_result_init(&result, &job, DAEMON_TX_OUTCOME_OK);
    result.completion_slot = 0;
    result.completion_generation = client_slot_generation(&slots[0]);
    daemon_tx_completion_queue_push(
        daemon_tx_async_runtime_completion_queue(),
        &result);

    expect_size("completion drain pending before",
                daemon_tx_async_runtime_completion_pending(),
                1);

    daemon_drain_framed_tx_completions("TESTF", slots, 2);

    expect_size("completion drain pending after",
                daemon_tx_async_runtime_completion_pending(),
                0);
    expect_int("completion drain read frame",
               (int)read(sv[1], frame, sizeof(frame)),
               (int)sizeof(frame));
    expect_int("completion drain type", frame[0], FRAMED_DATA_TYPE_TX_RESULT);
    expect_int("completion drain status", frame[3], FRAMED_DATA_TX_STATUS_OK);
    expect_int("completion drain seq lo", frame[5], 0x56);
    expect_int("completion drain seq hi", frame[6], 0x34);

    close(sv[1]);
    client_slot_close_all(slots, 2);
    daemon_tx_async_runtime_shutdown();
}


/* --- Default log line for a queued TX refused in the worker ------------ */

/* The drain runs under the band's framed log context: band->framed_log_ctx,
 * "TX868F" on 868 (daemon_band.h, daemon_socket_dispatch.cpp). With TXQUEUE=0
 * the unqueued path prints the same wording under the band tag ("868"). */
#define DRAIN_TAG "TX868F"

/* stdout into a pipe, kept for capture_end. 0 on success. */
static int capture_begin(int *saved, int *read_fd)
{
    int pfd[2];

    fflush(stdout);
    if (pipe(pfd) != 0)
        return -1;
    *saved = dup(STDOUT_FILENO);
    if (*saved < 0 || dup2(pfd[1], STDOUT_FILENO) < 0) {
        close(pfd[0]);
        close(pfd[1]);
        if (*saved >= 0)
            close(*saved);
        return -1;
    }
    close(pfd[1]);
    *read_fd = pfd[0];
    return 0;
}

/* Restore stdout; *out gets the text printed since capture_begin. */
static void capture_end(int saved, int read_fd, char *out, size_t out_len)
{
    size_t used = 0;
    ssize_t n;

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    while (used + 1 < out_len &&
           (n = read(read_fd, out + used, out_len - 1 - used)) > 0)
        used += (size_t)n;
    out[used] = '\0';
    close(read_fd);
}

/* Drain with stdout captured into *out. 0 on success. */
static int drain_captured(ClientSlot *slots, int max_clients,
                          char *out, size_t out_len)
{
    int saved;
    int read_fd;

    out[0] = '\0';
    if (capture_begin(&saved, &read_fd) != 0)
        return -1;
    daemon_drain_framed_tx_completions(DRAIN_TAG, slots, max_clients);
    capture_end(saved, read_fd, out, out_len);
    return 0;
}

static int count_text(const char *text, const char *needle)
{
    int count = 0;
    size_t len = strlen(needle);

    for (const char *p = strstr(text, needle); p; p = strstr(p + len, needle))
        count++;
    return count;
}

static void push_completion(uint8_t flags, DaemonTxOutcome outcome,
                            int slot, uint32_t generation)
{
    DaemonTxJob job = make_job(0x4242, flags);
    DaemonTxJobResult result;

    daemon_tx_job_result_init(&result, &job, outcome);
    result.completion_slot = slot;
    result.completion_generation = generation;
    daemon_tx_completion_queue_push(
        daemon_tx_async_runtime_completion_queue(),
        &result);
}

static void test_drain_logs_refusal_for_live_client(void)
{
    ClientSlot slots[1];
    int sv[2];
    char out[1024];
    uint8_t frame[FRAMED_DATA_HEADER_LEN + FRAMED_DATA_TX_RESULT_PAYLOAD_LEN];

    daemon_tx_async_runtime_shutdown();
    daemon_tx_async_runtime_init();
    client_slot_init_all(slots, 1);
    expect_int("refusal live socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    client_slot_set_fd(&slots[0], sv[0]);

    /* CAD wait expired in the worker: CHANNEL_BUSY -> TX_RESULT_CAD_TIMEOUT. */
    push_completion(FRAMED_DATA_TX_RESULT_FLAG_DEFERRED,
                    DAEMON_TX_OUTCOME_CHANNEL_BUSY,
                    0, client_slot_generation(&slots[0]));

    expect_int("refusal live capture", drain_captured(slots, 1, out, sizeof(out)), 0);
    expect_int("refusal live line",
               count_text(out, "[" DRAIN_TAG "] DATA TX aborted: CAD_TIMEOUT\n"), 1);
    expect_int("refusal live one line only",
               count_text(out, "DATA TX aborted"), 1);
    expect_int("refusal live TX_RESULT read",
               (int)read(sv[1], frame, sizeof(frame)), (int)sizeof(frame));
    expect_int("refusal live TX_RESULT type", frame[0], FRAMED_DATA_TYPE_TX_RESULT);
    expect_int("refusal live TX_RESULT status", frame[3],
               daemon_tx_outcome_to_framed_status(DAEMON_TX_OUTCOME_CHANNEL_BUSY));

    close(sv[1]);
    client_slot_close_all(slots, 1);
    daemon_tx_async_runtime_shutdown();
}

static void test_drain_logs_refusal_for_gone_client(void)
{
    ClientSlot slots[1];
    int old_sv[2];
    int new_sv[2];
    uint32_t old_generation;
    char out[1024];

    daemon_tx_async_runtime_shutdown();
    daemon_tx_async_runtime_init();
    client_slot_init_all(slots, 1);
    expect_int("refusal stale old socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, old_sv), 0);
    client_slot_set_fd(&slots[0], old_sv[0]);
    old_generation = client_slot_generation(&slots[0]);
    close(old_sv[1]);
    client_slot_close(&slots[0]);
    expect_int("refusal stale new socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, new_sv), 0);
    client_slot_set_fd(&slots[0], new_sv[0]);

    /* The sender is gone (its slot reused): no TX_RESULT, the line stays. */
    push_completion(FRAMED_DATA_TX_RESULT_FLAG_DEFERRED,
                    DAEMON_TX_OUTCOME_CHANNEL_BUSY, 0, old_generation);

    expect_int("refusal stale capture", drain_captured(slots, 1, out, sizeof(out)), 0);
    expect_int("refusal stale line",
               count_text(out, "[" DRAIN_TAG "] DATA TX aborted: CAD_TIMEOUT\n"), 1);
    expect_size("refusal stale counted stale",
                daemon_tx_async_runtime_completion_stale(), 1);
    expect_size("refusal stale nothing delivered",
                client_output_queue_pending(&slots[0].output), 0);

    close(new_sv[1]);
    client_slot_close_all(slots, 1);
    daemon_tx_async_runtime_shutdown();
}

static void test_drain_logs_radio_error_without_target(void)
{
    ClientSlot slots[1];
    char out[1024];

    daemon_tx_async_runtime_shutdown();
    daemon_tx_async_runtime_init();
    client_slot_init_all(slots, 1);

    /* A raw-socket job has no completion target. */
    push_completion(0, DAEMON_TX_OUTCOME_RADIO_ERROR,
                    DAEMON_TX_COMPLETION_SLOT_NONE, 0u);

    expect_int("radio error capture", drain_captured(slots, 1, out, sizeof(out)), 0);
    expect_int("radio error line",
               count_text(out, "[" DRAIN_TAG "] DATA TX aborted: RADIO_ERROR\n"), 1);

    client_slot_close_all(slots, 1);
    daemon_tx_async_runtime_shutdown();
}

static void test_drain_quiet_for_sent_jobs(void)
{
    ClientSlot slots[1];
    int sv[2];
    char out[1024];
    uint8_t frames[2 * (FRAMED_DATA_HEADER_LEN + FRAMED_DATA_TX_RESULT_PAYLOAD_LEN)];

    daemon_tx_async_runtime_shutdown();
    daemon_tx_async_runtime_init();
    client_slot_init_all(slots, 1);
    expect_int("sent quiet socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    client_slot_set_fd(&slots[0], sv[0]);

    /* Sent, and sent after a CAD timeout (CADTXAFTERTIMEOUT): no line. */
    push_completion(FRAMED_DATA_TX_RESULT_FLAG_DEFERRED,
                    DAEMON_TX_OUTCOME_OK, 0, client_slot_generation(&slots[0]));
    push_completion(FRAMED_DATA_TX_RESULT_FLAG_DEFERRED |
                        FRAMED_DATA_TX_RESULT_FLAG_CAD_TIMEOUT,
                    DAEMON_TX_OUTCOME_OK, 0, client_slot_generation(&slots[0]));

    expect_int("sent quiet capture", drain_captured(slots, 1, out, sizeof(out)), 0);
    expect_int("sent quiet no line", count_text(out, "DATA TX aborted"), 0);
    expect_int("sent quiet both TX_RESULTs",
               (int)read(sv[1], frames, sizeof(frames)), (int)sizeof(frames));

    close(sv[1]);
    client_slot_close_all(slots, 1);
    daemon_tx_async_runtime_shutdown();
}


static void test_frame_error_logged(void)
{
    ClientSlot slots[1];
    FramedDataTxState states[1];
    EventLoopSet set;
    EventLoopReadySet ready;
    int sv[2];
    int saved;
    int read_fd;
    char out[1024];
    const char *msg = "unknown frame type";
    const uint8_t bad_frame[FRAMED_DATA_HEADER_LEN] = { 0x7f, 0x00, 0x00 };

    client_slot_init_all(slots, 1);
    framed_data_tx_state_init_all(states, 1);
    expect_int("frame error socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    client_slot_set_fd(&slots[0], sv[0]);
    expect_int("frame error write",
               (int)write(sv[1], bad_frame, sizeof(bad_frame)),
               (int)sizeof(bad_frame));
    expect_int("frame error loop init", event_loop_init(&set), 0);
    event_loop_add_fd(&set, sv[0]);
    expect_int("frame error readable", event_loop_wait(&set, &ready, 1000000), 1);

    out[0] = '\0';
    expect_int("frame error capture", capture_begin(&saved, &read_fd), 0);
    daemon_process_framed_data_slots(DRAIN_TAG, slots, states, 1, &ready,
                                     NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    capture_end(saved, read_fd, out, sizeof(out));

    expect_int("frame error line",
               count_text(out, "[" DRAIN_TAG "] DATA frame error: unknown frame type\n"), 1);
    expect_int("frame error one line only", count_text(out, "DATA frame error"), 1);
    expect_size("frame error ERROR frame queued",
                client_output_queue_pending(&slots[0].output),
                FRAMED_DATA_HEADER_LEN + strlen(msg));

    event_loop_close(&set);
    close(sv[1]);
    client_slot_close_all(slots, 1);
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

    test_framed_immediate_result_policy();
    test_frame_len();
    test_encode_ok_frame();
    test_encode_error_frame();
    test_completion_queue_push_pop();
    test_completion_queue_drops_oldest_when_full();
    test_completion_delivers_to_target_slot();
    test_completion_ignores_reused_slot_generation();
    test_completion_ignores_missing_target();
    test_drain_counts_stale_completion();
    test_drain_delivers_queued_completion();
    test_drain_logs_refusal_for_live_client();
    test_drain_logs_refusal_for_gone_client();
    test_drain_logs_radio_error_without_target();
    test_drain_quiet_for_sent_jobs();
    test_frame_error_logged();
    test_encode_rejects_bad_args();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
