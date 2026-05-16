#!/usr/bin/env bash
set -euo pipefail

REF_DIR="loradaemon_refactor"
TEST_DIR="$REF_DIR/tests"

if [[ ! -d .git ]]; then
  echo "ERROR: bitte im Root des Git-Repositories ausführen." >&2
  exit 1
fi

if [[ ! -d "$REF_DIR" ]]; then
  echo "ERROR: $REF_DIR nicht gefunden." >&2
  exit 1
fi

mkdir -p "$TEST_DIR"

echo "[1/6] Rename existing monolithic test to baseline test..."

if [[ -f "$TEST_DIR/test_loradaemon_320_108_interface.c" ]]; then
  git mv "$TEST_DIR/test_loradaemon_320_108_interface.c" "$TEST_DIR/test_interface_baseline.c" 2>/dev/null || \
  mv "$TEST_DIR/test_loradaemon_320_108_interface.c" "$TEST_DIR/test_interface_baseline.c"
elif [[ -f "$TEST_DIR/test_interface_baseline.c" ]]; then
  echo "INFO: test_interface_baseline.c already exists."
else
  echo "ERROR: existing test source not found in $TEST_DIR" >&2
  exit 1
fi

echo "[2/6] Write common_loradaemon_test.h..."

cat > "$TEST_DIR/common_loradaemon_test.h" <<'EOF'
#ifndef COMMON_LORADAEMON_TEST_H
#define COMMON_LORADAEMON_TEST_H

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define SOCK_DATA_433 "/tmp/lora433.sock"
#define SOCK_DATA_868 "/tmp/lora868.sock"
#define SOCK_CONF_433 "/tmp/loraconf433.sock"
#define SOCK_CONF_868 "/tmp/loraconf868.sock"

#define DEFAULT_SOCKET_TIMEOUT_MS 8000
#define DEFAULT_RSSI_TIMEOUT_MS   4000

#define TEST_PASS   0
#define TEST_FAIL  -1
#define TEST_SKIP   1

static int g_ok = 0;
static int g_fail = 0;
static int g_skip = 0;
static int g_xfail = 0;
static int g_xpass = 0;

static pid_t g_daemon_pid = -1;

static const char *lora_cfg_433 =
    "SET MODE=LORA FREQ=433.775 SF=12 BW=125 CR=5 "
    "CRC=1 PREAMBLE=8 SYNC=0x12 LDRO=1 POWER=17\n";

static const char *lora_cfg_868 =
    "SET MODE=LORA FREQ=869.525 SF=11 BW=250 CR=5 "
    "CRC=1 PREAMBLE=16 SYNC=0x2B LDRO=AUTO POWER=10\n";

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void fail_msg(const char *fmt, ...)
{
    va_list ap;
    printf("[FAIL] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void info_msg(const char *fmt, ...)
{
    va_list ap;
    printf("[INFO] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int connect_unix_once(const char *path)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int connect_unix_retry(const char *path, int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        int fd = connect_unix_once(path);
        if (fd >= 0)
            return fd;
        usleep(50000);
    }

    return -1;
}

static int wait_for_socket(const char *path, int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        if (path_exists(path)) {
            int fd = connect_unix_retry(path, 300);
            if (fd >= 0) {
                close(fd);
                return 0;
            }
        }
        usleep(100000);
    }

    return -1;
}

static int wait_all_sockets(int timeout_ms)
{
    const char *paths[] = {
        SOCK_DATA_433,
        SOCK_DATA_868,
        SOCK_CONF_433,
        SOCK_CONF_868
    };

    for (int i = 0; i < ARRAY_LEN(paths); i++) {
        if (wait_for_socket(paths[i], timeout_ms) < 0) {
            fail_msg("socket not ready: %s", paths[i]);
            return TEST_FAIL;
        }
    }

    return TEST_PASS;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;

        p += n;
        len -= (size_t)n;
    }

    return 0;
}

static int line_matches_regex(const char *line, const char *pattern)
{
    regex_t rx;
    int ret;

    if (regcomp(&rx, pattern, REG_EXTENDED | REG_NOSUB) != 0)
        return 0;

    ret = regexec(&rx, line, 0, NULL, 0);
    regfree(&rx);

    return ret == 0;
}

static int wait_for_matching_line(int fd, const char *pattern,
                                  int timeout_ms, char *out, size_t out_size)
{
    char buf[4096];
    char line[512];
    size_t line_len = 0;
    long deadline = now_ms() + timeout_ms;

    memset(line, 0, sizeof(line));

    while (now_ms() < deadline) {
        struct pollfd pfd;
        int remaining = (int)(deadline - now_ms());
        int pr;

        if (remaining < 1)
            remaining = 1;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        pr = poll(&pfd, 1, remaining);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (pr == 0)
            continue;

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;

        if (pfd.revents & POLLIN) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0)
                return -1;

            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];

                if (c == '\n') {
                    line[line_len] = '\0';

                    while (line_len > 0 &&
                           (line[line_len - 1] == '\r' ||
                            line[line_len - 1] == ' ' ||
                            line[line_len - 1] == '\t')) {
                        line[--line_len] = '\0';
                    }

                    if (line_matches_regex(line, pattern)) {
                        if (out && out_size > 0) {
                            strncpy(out, line, out_size - 1);
                            out[out_size - 1] = '\0';
                        }
                        return 0;
                    }

                    line_len = 0;
                    line[0] = '\0';
                } else {
                    if (line_len + 1 < sizeof(line))
                        line[line_len++] = c;
                    else
                        line_len = 0;
                }
            }
        }
    }

    return -1;
}

static int start_daemon(const char *bin)
{
    pid_t pid = fork();

    if (pid < 0)
        return TEST_FAIL;

    if (pid == 0) {
        setpgid(0, 0);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }

    g_daemon_pid = pid;
    return TEST_PASS;
}

static void stop_daemon(void)
{
    int status;

    if (g_daemon_pid <= 0)
        return;

    kill(-g_daemon_pid, SIGTERM);

    for (int i = 0; i < 30; i++) {
        pid_t r = waitpid(g_daemon_pid, &status, WNOHANG);
        if (r == g_daemon_pid) {
            g_daemon_pid = -1;
            return;
        }
        usleep(100000);
    }

    kill(-g_daemon_pid, SIGKILL);
    waitpid(g_daemon_pid, &status, 0);
    g_daemon_pid = -1;
}

static int daemon_alive(void)
{
    if (g_daemon_pid <= 0)
        return 1;

    return kill(g_daemon_pid, 0) == 0;
}

static void run_test(const char *name, int (*fn)(void))
{
    int ret;

    printf("[TEST] %s\n", name);
    ret = fn();

    if (ret == TEST_PASS) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else if (ret == TEST_SKIP) {
        g_skip++;
        printf("[SKIP] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
}

static void run_xfail_test(const char *name, int (*fn)(void))
{
    int ret;

    printf("[TEST] %s\n", name);
    ret = fn();

    if (ret == TEST_PASS) {
        g_xpass++;
        printf("[XPASS] %s\n", name);
    } else {
        g_xfail++;
        printf("[XFAIL] %s\n", name);
    }
}

static int print_summary(void)
{
    printf("\nSummary: ok=%d fail=%d skip=%d xfail=%d xpass=%d\n",
           g_ok, g_fail, g_skip, g_xfail, g_xpass);

    return g_fail ? 1 : 0;
}

static void usage_common(const char *argv0)
{
    printf("Usage: %s --bin ./loraham_daemon\n", argv0);
}

#endif
EOF

echo "[3/6] Write test_config_stream.c..."

cat > "$TEST_DIR/test_config_stream.c" <<'EOF'
#include "common_loradaemon_test.h"

static const char *g_bin = NULL;

static int test_persistent_conf_commands_433(void)
{
    int fd = connect_unix_retry(SOCK_CONF_433, 2000);
    char line[128];

    if (fd < 0)
        return TEST_FAIL;

    if (write_all(fd, "SET GETRSSI=1\n", strlen("SET GETRSSI=1\n")) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    if (wait_for_matching_line(fd,
                               "^RSSI=-?[0-9]+\\.[0-9][0-9]$",
                               DEFAULT_RSSI_TIMEOUT_MS,
                               line,
                               sizeof(line)) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    printf("       433 RSSI line: %s\n", line);

    if (write_all(fd, "SET GETRSSI=0\n", strlen("SET GETRSSI=0\n")) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    usleep(200000);

    if (write_all(fd, lora_cfg_433, strlen(lora_cfg_433)) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    usleep(200000);
    close(fd);

    return daemon_alive() ? TEST_PASS : TEST_FAIL;
}

static int test_rssi_reconnect_433(void)
{
    int fd;

    fd = connect_unix_retry(SOCK_CONF_433, 2000);
    if (fd < 0)
        return TEST_FAIL;

    if (write_all(fd, "SET GETRSSI=1\n", strlen("SET GETRSSI=1\n")) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    if (wait_for_matching_line(fd,
                               "^RSSI=-?[0-9]+\\.[0-9][0-9]$",
                               DEFAULT_RSSI_TIMEOUT_MS,
                               NULL,
                               0) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    close(fd);
    usleep(500000);

    fd = connect_unix_retry(SOCK_CONF_433, 2000);
    if (fd < 0)
        return TEST_FAIL;

    if (write_all(fd, "SET GETRSSI=1\n", strlen("SET GETRSSI=1\n")) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    if (wait_for_matching_line(fd,
                               "^RSSI=-?[0-9]+\\.[0-9][0-9]$",
                               DEFAULT_RSSI_TIMEOUT_MS,
                               NULL,
                               0) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    (void)write_all(fd, "SET GETRSSI=0\n", strlen("SET GETRSSI=0\n"));
    close(fd);

    return TEST_PASS;
}

static int test_parallel_rssi_433_868(void)
{
    int fd433;
    int fd868;

    fd433 = connect_unix_retry(SOCK_CONF_433, 2000);
    fd868 = connect_unix_retry(SOCK_CONF_868, 2000);

    if (fd433 < 0 || fd868 < 0) {
        if (fd433 >= 0)
            close(fd433);
        if (fd868 >= 0)
            close(fd868);
        return TEST_FAIL;
    }

    if (write_all(fd433, "SET GETRSSI=1\n", strlen("SET GETRSSI=1\n")) < 0 ||
        write_all(fd868, "SET GETRSSI=1\n", strlen("SET GETRSSI=1\n")) < 0) {
        close(fd433);
        close(fd868);
        return TEST_FAIL;
    }

    if (wait_for_matching_line(fd433,
                               "^RSSI=-?[0-9]+\\.[0-9][0-9]$",
                               DEFAULT_RSSI_TIMEOUT_MS,
                               NULL,
                               0) < 0) {
        close(fd433);
        close(fd868);
        return TEST_FAIL;
    }

    if (wait_for_matching_line(fd868,
                               "^RSSI=-?[0-9]+\\.[0-9][0-9]$",
                               DEFAULT_RSSI_TIMEOUT_MS,
                               NULL,
                               0) < 0) {
        close(fd433);
        close(fd868);
        return TEST_FAIL;
    }

    (void)write_all(fd433, "SET GETRSSI=0\n", strlen("SET GETRSSI=0\n"));
    (void)write_all(fd868, "SET GETRSSI=0\n", strlen("SET GETRSSI=0\n"));

    close(fd433);
    close(fd868);

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

    run_test("persistent conf connection 433", test_persistent_conf_commands_433);
    run_test("RSSI reconnect 433", test_rssi_reconnect_433);
    run_test("parallel RSSI 433 + 868", test_parallel_rssi_433_868);

    if (!daemon_alive()) {
        fail_msg("daemon exited during test");
        g_fail++;
    }

    stop_daemon();
    return print_summary();
}
EOF

echo "[4/6] Write test_client_lifecycle.c..."

cat > "$TEST_DIR/test_client_lifecycle.c" <<'EOF'
#include "common_loradaemon_test.h"

static const char *g_bin = NULL;

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

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bin") == 0) {
            if (i + 1 >= argc) {
                usage_common(argv[0]);
                return 2;
            }
            g_bin = argv[++i];
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
EOF

echo "[5/6] Write test_known_issues.c..."

cat > "$TEST_DIR/test_known_issues.c" <<'EOF'
#include "common_loradaemon_test.h"

static const char *g_bin = NULL;

static int desired_fragmented_conf_command_433(void)
{
    int fd = connect_unix_retry(SOCK_CONF_433, 2000);

    if (fd < 0)
        return TEST_FAIL;

    if (write_all(fd, "SET GETRSSI=", strlen("SET GETRSSI=")) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    usleep(250000);

    if (write_all(fd, "1\n", strlen("1\n")) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    if (wait_for_matching_line(fd,
                               "^RSSI=-?[0-9]+\\.[0-9][0-9]$",
                               DEFAULT_RSSI_TIMEOUT_MS,
                               NULL,
                               0) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    (void)write_all(fd, "SET GETRSSI=0\n", strlen("SET GETRSSI=0\n"));
    close(fd);

    return TEST_PASS;
}

static int desired_multiple_commands_one_write_433(void)
{
    int fd = connect_unix_retry(SOCK_CONF_433, 2000);
    const char *cmd = "BOGUS\nSET GETRSSI=1\n";

    if (fd < 0)
        return TEST_FAIL;

    if (write_all(fd, cmd, strlen(cmd)) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    if (wait_for_matching_line(fd,
                               "^RSSI=-?[0-9]+\\.[0-9][0-9]$",
                               DEFAULT_RSSI_TIMEOUT_MS,
                               NULL,
                               0) < 0) {
        close(fd);
        return TEST_FAIL;
    }

    (void)write_all(fd, "SET GETRSSI=0\n", strlen("SET GETRSSI=0\n"));
    close(fd);

    return TEST_PASS;
}

static int desired_strict_numeric_validation(void)
{
    /*
     * Known issue placeholder:
     * Current daemon has no stable OK/ERR response contract for invalid
     * numeric values like POWER=abc, CRC=abc, SF=12xyz, BW=125foo.
     */
    return TEST_FAIL;
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

    run_xfail_test("desired: fragmented config command is line-buffered",
                   desired_fragmented_conf_command_433);
    run_xfail_test("desired: multiple commands in one write are processed",
                   desired_multiple_commands_one_write_433);
    run_xfail_test("desired: strict numeric validation has observable errors",
                   desired_strict_numeric_validation);

    if (!daemon_alive()) {
        fail_msg("daemon exited during known-issue tests");
        g_fail++;
    }

    stop_daemon();
    return print_summary();
}
EOF

echo "[6/6] Patch build.sh, run_tests.sh and .gitignore..."

cat > "$REF_DIR/build.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
TEST_DIR="$SCRIPT_DIR/tests"

DAEMON_SRC="$SCRIPT_DIR/loradaemon_320_108.cpp"
DAEMON_OUT="$SCRIPT_DIR/loraham_daemon"

cc="${CC:-gcc}"
cxx="${CXX:-g++}"

radiolib_cflags=()
radiolib_libs=()

try_source_radiolib_dir() {
  local dir="$1"

  [[ -n "$dir" ]] || return 1
  [[ -d "$dir/src" ]] || return 1
  [[ -f "$dir/src/RadioLib.h" ]] || return 1
  [[ -f "$dir/src/hal/RPi/PiHal.h" ]] || return 1
  [[ -f "$dir/build/libRadioLib.a" ]] || return 1

  radiolib_cflags=(
    -I"$dir/src"
    -I"$dir/src/hal"
    -I"$dir/src/modules"
    -I"$dir/src/protocols/PhysicalLayer"
  )

  radiolib_libs=(
    "$dir/build/libRadioLib.a"
  )

  echo "Using RadioLib source tree: $dir"
  return 0
}

try_installed_radiolib_prefix() {
  local prefix="$1"
  local inc=""
  local lib=""

  [[ -n "$prefix" ]] || return 1

  if [[ -f "$prefix/include/RadioLib.h" && -f "$prefix/include/hal/RPi/PiHal.h" ]]; then
    inc="$prefix/include"
  elif [[ -f "$prefix/include/RadioLib/RadioLib.h" && -f "$prefix/include/RadioLib/hal/RPi/PiHal.h" ]]; then
    inc="$prefix/include/RadioLib"
  else
    return 1
  fi

  if [[ -f "$prefix/lib/libRadioLib.a" ]]; then
    lib="$prefix/lib/libRadioLib.a"
  elif [[ -f "$prefix/lib/aarch64-linux-gnu/libRadioLib.a" ]]; then
    lib="$prefix/lib/aarch64-linux-gnu/libRadioLib.a"
  elif [[ -f "$prefix/lib/libRadioLib.so" || -f "$prefix/lib/aarch64-linux-gnu/libRadioLib.so" ]]; then
    radiolib_cflags=(-I"$inc")
    radiolib_libs=(-L"$prefix/lib" -L"$prefix/lib/aarch64-linux-gnu" -lRadioLib)
    echo "Using installed RadioLib: $prefix"
    return 0
  else
    return 1
  fi

  radiolib_cflags=(-I"$inc")
  radiolib_libs=("$lib")

  echo "Using installed RadioLib: $prefix"
  return 0
}

find_radiolib() {
  if [[ -n "${RADIOLIB_DIR:-}" ]]; then
    try_source_radiolib_dir "$RADIOLIB_DIR" && return 0
    echo "ERROR: RADIOLIB_DIR is set but not usable: $RADIOLIB_DIR" >&2
    echo "Expected: src/RadioLib.h, src/hal/RPi/PiHal.h and build/libRadioLib.a" >&2
    return 1
  fi

  local candidates=(
    "$HOME/RadioLib"
    "$HOME/src/RadioLib"
    "$HOME/src/radiolib"
    "$REPO_ROOT/../RadioLib"
    "$REPO_ROOT/../../RadioLib"
    "/home/raspberry/RadioLib"
    "/home/pi/RadioLib"
  )

  for dir in "${candidates[@]}"; do
    try_source_radiolib_dir "$dir" && return 0
  done

  try_installed_radiolib_prefix "/usr/local" && return 0
  try_installed_radiolib_prefix "/usr" && return 0

  return 1
}

build_daemon() {
  if [[ ! -f "$DAEMON_SRC" ]]; then
    echo "ERROR: daemon source file not found: $DAEMON_SRC" >&2
    exit 1
  fi

  if ! find_radiolib; then
    echo "ERROR: RadioLib not found." >&2
    echo "" >&2
    echo "Try locating it:" >&2
    echo "  find \"\$HOME\" -maxdepth 4 -name libRadioLib.a 2>/dev/null" >&2
    echo "" >&2
    echo "Then run for example:" >&2
    echo "  RADIOLIB_DIR=\$HOME/src/RadioLib $0" >&2
    echo "" >&2
    echo "Or build RadioLib first:" >&2
    echo "  git clone https://github.com/jgromes/RadioLib \$HOME/src/RadioLib" >&2
    echo "  cd \$HOME/src/RadioLib" >&2
    echo "  mkdir -p build && cd build" >&2
    echo "  cmake .." >&2
    echo "  make" >&2
    exit 1
  fi

  "$cxx" \
    -std=c++11 \
    -O2 \
    -o "$DAEMON_OUT" \
    "$DAEMON_SRC" \
    "${radiolib_cflags[@]}" \
    "${radiolib_libs[@]}" \
    -llgpio

  echo "Built daemon: $DAEMON_OUT"
}

build_tests() {
  local src
  local out

  for src in "$TEST_DIR"/test_*.c; do
    out="${src%.c}"

    "$cc" \
      -std=c11 \
      -Wall \
      -Wextra \
      -O2 \
      -I"$TEST_DIR" \
      -o "$out" \
      "$src"

    echo "Built test:   $out"
  done
}

case "${1:-all}" in
  all)
    build_daemon
    build_tests
    ;;
  daemon)
    build_daemon
    ;;
  test|tests)
    build_tests
    ;;
  *)
    echo "Usage: $0 [all|daemon|test]" >&2
    exit 2
    ;;
esac
EOF

cat > "$REF_DIR/run_tests.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="$SCRIPT_DIR/tests"
DAEMON_BIN="$SCRIPT_DIR/loraham_daemon"

tx_tests=false
rx_seconds="${RX_SECONDS:-15}"

usage() {
  cat <<EOF_HELP
Usage: ./loradaemon_refactor/run_tests.sh [--TX] [--rx-seconds N]

Builds the daemon and test binaries, then runs each test with its own daemon.

Options:
  --TX             Run RF transmit tests too
  --rx-seconds N   RX observation time for --TX, default: 15
  -h, --help       Show this help
EOF_HELP
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --TX)
      tx_tests=true
      shift
      ;;
    --rx-seconds)
      if [[ $# -lt 2 ]]; then
        echo "ERROR: --rx-seconds needs a value." >&2
        exit 2
      fi
      rx_seconds="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown option: $1" >&2
      echo >&2
      usage >&2
      exit 2
      ;;
  esac
done

if pgrep -x loraham_daemon >/dev/null; then
  echo "ERROR: loraham_daemon is already running:"
  pgrep -af loraham_daemon
  echo
  echo "Stop it first, then run this script again."
  exit 1
fi

"$SCRIPT_DIR/build.sh"

tests=(
  "$TEST_DIR/test_interface_baseline"
  "$TEST_DIR/test_config_stream"
  "$TEST_DIR/test_client_lifecycle"
  "$TEST_DIR/test_known_issues"
)

for test_bin in "${tests[@]}"; do
  echo
  echo "================================================================"
  echo "Running: $test_bin"
  echo "================================================================"

  cmd=("$test_bin" --bin "$DAEMON_BIN")

  if [[ "$test_bin" == "$TEST_DIR/test_interface_baseline" && "$tx_tests" == true ]]; then
    cmd+=(--rf-tx --rx-seconds "$rx_seconds")
  fi

  "${cmd[@]}"
done
EOF

cat > "$REF_DIR/.gitignore" <<'EOF'
# Built test binaries
/tests/test_interface_baseline
/tests/test_config_stream
/tests/test_client_lifecycle
/tests/test_known_issues
/tests/test_loradaemon_interface
/tests/test_loradaemon_320_108_interface

# Optional local daemon binaries built inside this refactor directory
/loraham_daemon
/loradaemon_320_108

# Object files and local build leftovers
*.o
*.a
*.so
*.d
*.log
core
core.*

# Local editor/temp files
*~
*.swp
*.swo
EOF

chmod +x "$REF_DIR/build.sh" "$REF_DIR/run_tests.sh"

echo
echo "Done."
echo
echo "Neue Struktur:"
find "$REF_DIR" -maxdepth 2 -type f | sort
echo
echo "Git status:"
git status --short
echo
echo "Jetzt testen:"
echo "  ./$REF_DIR/run_tests.sh"
echo
echo "Danach committen:"
echo "  git add -A"
echo "  git commit -m \"Split loradaemon refactor tests\""
echo "  git push origin hardening/daemon-tests"
