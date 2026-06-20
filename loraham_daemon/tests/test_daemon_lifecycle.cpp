#include "../daemon_lifecycle.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Daemon lifecycle tests.
 *
 * These lock stop-request behavior before signal handling is wired into the
 * daemon main loop.
 */

static int g_ok = 0;
static int g_fail = 0;

/* --- Test helpers --- */

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

/* --- Stop request behavior --- */

static void test_stop_flag_reset_and_request(void)
{
    daemon_lifecycle_reset_stop();
    expect_int("stop initially clear", daemon_lifecycle_stop_requested(), 0);

    daemon_lifecycle_request_stop(SIGTERM);
    expect_int("stop requested after handler", daemon_lifecycle_stop_requested(), 1);

    daemon_lifecycle_reset_stop();
    expect_int("stop clear after reset", daemon_lifecycle_stop_requested(), 0);
}

static void test_stop_request_is_idempotent(void)
{
    daemon_lifecycle_reset_stop();

    daemon_lifecycle_request_stop(SIGINT);
    daemon_lifecycle_request_stop(SIGTERM);

    expect_int("stop request idempotent", daemon_lifecycle_stop_requested(), 1);
}

static void test_signal_install_and_raise(void)
{
    daemon_lifecycle_reset_stop();

    expect_int("signal handler install",
               daemon_lifecycle_install_signal_handlers(), 0);

    raise(SIGTERM);

    expect_int("SIGTERM requests stop", daemon_lifecycle_stop_requested(), 1);

    daemon_lifecycle_reset_stop();
}

/* --- Daemon stdio behavior --- */

static void test_stdio_redirect_rejects_empty_path(void)
{
    errno = 0;

    expect_int("stdio redirect rejects null path",
               daemon_lifecycle_redirect_stdio(NULL), -1);
    expect_int("null path sets EINVAL", errno, EINVAL);
}

static void test_stdio_redirect_appends_and_flushes(void)
{
    const char before[] = "before\n";
    char path[] = "/tmp/loraham-lifecycle.XXXXXX";
    char content[256];
    FILE *file;
    pid_t pid;
    int fd;
    int status;
    size_t count;

    fd = mkstemp(path);
    if (fd < 0) {
        g_fail++;
        printf("[FAIL] stdio redirect temp file\n");
        return;
    }

    if (write(fd, before, sizeof(before) - 1) != (ssize_t)(sizeof(before) - 1)) {
        close(fd);
        unlink(path);
        g_fail++;
        printf("[FAIL] stdio redirect seed write\n");
        return;
    }

    close(fd);
    fflush(NULL);

    pid = fork();
    if (pid < 0) {
        unlink(path);
        g_fail++;
        printf("[FAIL] stdio redirect fork\n");
        return;
    }

    if (pid == 0) {
        if (daemon_lifecycle_redirect_stdio(path) != 0)
            _exit(1);

        printf("stdout line\n");
        fprintf(stderr, "stderr line\n");
        _exit(0);
    }

    if (waitpid(pid, &status, 0) < 0) {
        unlink(path);
        g_fail++;
        printf("[FAIL] stdio redirect wait\n");
        return;
    }

    expect_int("stdio redirect child exits",
               WIFEXITED(status) && WEXITSTATUS(status) == 0, 1);

    file = fopen(path, "r");
    if (!file) {
        unlink(path);
        g_fail++;
        printf("[FAIL] stdio redirect log read\n");
        return;
    }

    count = fread(content, 1, sizeof(content) - 1, file);
    if (ferror(file)) {
        fclose(file);
        unlink(path);
        g_fail++;
        printf("[FAIL] stdio redirect log content\n");
        return;
    }

    fclose(file);
    unlink(path);
    content[count] = '\0';

    expect_int("stdio redirect keeps existing log", strstr(content, before) != NULL, 1);
    expect_int("stdio redirect flushes stdout", strstr(content, "stdout line\n") != NULL, 1);
    expect_int("stdio redirect flushes stderr", strstr(content, "stderr line\n") != NULL, 1);
}

/* --- CLI parsing and test sequence --- */

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

    test_stop_flag_reset_and_request();
    test_stop_request_is_idempotent();
    test_stdio_redirect_rejects_empty_path();
    test_stdio_redirect_appends_and_flushes();
    test_signal_install_and_raise();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
