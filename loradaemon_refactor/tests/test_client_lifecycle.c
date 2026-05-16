#include "common_loradaemon_test.h"

/*
 * Client lifecycle tests.
 *
 * These tests exercise connect/disconnect behavior and fixed client-slot
 * handling without sending RF payloads.
 */

static const char *g_bin = NULL;

/* --- Repeated short-lived connections on all public sockets --- */

static int test_repeated_connect_disconnect_all_sockets(void)
{
    const char *paths[] = {
        SOCK_DATA_433,
        SOCK_DATA_868,
        SOCK_CONF_433,
        SOCK_CONF_868
    };

    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < ARRAY_LEN(paths); i++) {
            int fd = connect_unix_retry(paths[i], 2000);
            if (fd < 0) {
                fail_msg("cannot connect on round %d: %s", round, paths[i]);
                return TEST_FAIL;
            }
            close(fd);
        }
    }

    return TEST_PASS;
}

/* --- Fill the current MAX_CLIENTS-sized conf client table --- */

static int test_many_conf_clients_433(void)
{
    int fds[12];
    int opened = 0;

    for (int i = 0; i < ARRAY_LEN(fds); i++)
        fds[i] = -1;

    for (int i = 0; i < ARRAY_LEN(fds); i++) {
        fds[i] = connect_unix_retry(SOCK_CONF_433, 1000);
        if (fds[i] >= 0)
            opened++;
    }

    if (opened < 10) {
        for (int i = 0; i < ARRAY_LEN(fds); i++) {
            if (fds[i] >= 0)
                close(fds[i]);
        }

        fail_msg("expected at least 10 clients, got %d", opened);
        return TEST_FAIL;
    }

    for (int i = 0; i < ARRAY_LEN(fds); i++) {
        if (fds[i] >= 0)
            close(fds[i]);
    }

    usleep(300000);

    return daemon_alive() ? TEST_PASS : TEST_FAIL;
}

/* --- After client pressure, normal reconnects must still work --- */

static int test_reconnect_after_client_storm(void)
{
    int fd;

    fd = connect_unix_retry(SOCK_CONF_433, 2000);
    if (fd < 0)
        return TEST_FAIL;
    close(fd);

    fd = connect_unix_retry(SOCK_DATA_433, 2000);
    if (fd < 0)
        return TEST_FAIL;
    close(fd);

    return TEST_PASS;
}

/* --- CLI parsing and test sequence --- */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bin") == 0) {
            if (i + 1 >= argc) {
                usage_common(argv[0]);
                return 2;
            }
            g_bin = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage_common(argv[0]);
            return 0;
        } else {
            usage_common(argv[0]);
            return 2;
        }
    }

    if (!g_bin) {
        usage_common(argv[0]);
        return 2;
    }

    info_msg("starting daemon: %s", g_bin);
    if (start_daemon(g_bin) < 0)
        return 1;

    if (wait_all_sockets(DEFAULT_SOCKET_TIMEOUT_MS) < 0) {
        stop_daemon();
        return 1;
    }

    run_test("repeated connect/disconnect all sockets",
             test_repeated_connect_disconnect_all_sockets);
    run_test("many conf clients 433", test_many_conf_clients_433);
    run_test("reconnect after client storm", test_reconnect_after_client_storm);

    if (!daemon_alive()) {
        fail_msg("daemon exited during test");
        g_fail++;
    }

    stop_daemon();
    return print_summary();
}
