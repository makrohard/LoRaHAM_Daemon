#include "../radio_controller.h"

#include <stdio.h>
#include <string.h>

/* --- Radio controller TX worker ownership tests ------------------------- */

static int g_ok = 0;
static int g_fail = 0;

struct FakeRadio {
    int dummy;
};

static void fake_rx_callback(void)
{
}

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

static DaemonTxJob make_job(uint16_t seq)
{
    DaemonTxJob job;
    uint8_t payload[] = { 0x55 };

    daemon_tx_job_init(&job, 433, RADIO_TX_MODE_MANAGED, seq);
    daemon_tx_job_set_payload(&job, payload, sizeof(payload));

    return job;
}

static void test_controller_initializes_worker(void)
{
    RadioController<FakeRadio> ctrl;

    radio_controller_init(&ctrl,
                          RADIO_BAND_433,
                          "TEST",
                          false,
                          fake_rx_callback,
                          13);

    expect_size("controller worker pending zero",
                daemon_tx_worker_pending(&ctrl.tx_worker), 0);
    expect_size("controller worker accepted zero",
                daemon_tx_worker_accepted(&ctrl.tx_worker), 0);
    expect_size("controller worker rejected zero",
                daemon_tx_worker_rejected(&ctrl.tx_worker), 0);
    expect_size("controller worker processed zero",
                daemon_tx_worker_processed(&ctrl.tx_worker), 0);
}

static void test_controller_worker_usable(void)
{
    RadioController<FakeRadio> ctrl;
    DaemonTxJob job = make_job(7);

    radio_controller_init(&ctrl,
                          RADIO_BAND_433,
                          "TEST",
                          false,
                          fake_rx_callback,
                          13);

    expect_int("controller worker submit",
               daemon_tx_worker_submit(&ctrl.tx_worker, &job), 0);
    expect_size("controller worker pending one",
                daemon_tx_worker_pending(&ctrl.tx_worker), 1);
    expect_size("controller worker accepted one",
                daemon_tx_worker_accepted(&ctrl.tx_worker), 1);
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

    test_controller_initializes_worker();
    test_controller_worker_usable();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
