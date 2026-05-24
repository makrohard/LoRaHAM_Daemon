#include "common_loradaemon_test.h"

/*
 * CONF status query behavior.
 *
 * GET STATUS returns one snapshot line for the selected radio. Status/config
 * remains on the CONF socket; DATA sockets stay payload-only.
 */

static const char *g_bin = NULL;

static int start_daemon_433(const char *bin)
{
    pid_t pid = fork();

    if (pid < 0)
        return TEST_FAIL;

    if (pid == 0) {
        setpgid(0, 0);
        execl(bin, bin, "--radio", "433", (char *)NULL);
        _exit(127);
    }

    g_daemon_pid = pid;
    return TEST_PASS;
}

static int test_get_status_433(void)
{
    int fd;
    char line[160];

    fd = connect_unix_retry(SOCK_CONF_433, 2000);
    if (fd < 0)
        return TEST_FAIL;

    if (write_all(fd, "GET STATUS\n", strlen("GET STATUS\n")) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    if (wait_for_matching_line(fd,
                               "^STATUS RADIO=(READY|FAILED|UNINITIALIZED) TX=[01] CAD=[01] GETRSSI=[01]$",
                               2000,
                               line,
                               sizeof(line)) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    printf("       433 status line: %s\n", line);

    close(fd);
    return TEST_PASS;
}

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

    info_msg("starting daemon: %s --radio 433", g_bin);
    if (start_daemon_433(g_bin) < 0)
        return 1;

    if (wait_for_socket(SOCK_CONF_433, DEFAULT_SOCKET_TIMEOUT_MS) < 0) {
        stop_daemon();
        return 1;
    }

    run_test("GET STATUS on CONF 433", test_get_status_433);

    if (!daemon_alive()) {
        fail_msg("daemon exited during test");
        g_fail++;
    }

    stop_daemon();
    return print_summary();
}
