#define _POSIX_C_SOURCE 200809L
#include "bridge_runtime.h"

#include "bridge_rx.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*
 * Generic runtime helpers for the bridge coordinator.
 */

static volatile sig_atomic_t bridge_stop_requested;

#ifdef LHKT_TEST
static size_t runtime_test_sleep_call_count;
#endif

static void bridge_runtime_signal_handler(int signo)
{
    (void)signo;
    bridge_stop_requested = 1;
}

void bridge_runtime_reset_stop_requested(void)
{
    bridge_stop_requested = 0;
}

int bridge_runtime_should_stop(void)
{
    return bridge_stop_requested != 0;
}

void bridge_runtime_install_signal_handlers(void)
{
    if (signal(SIGINT, bridge_runtime_signal_handler) == SIG_ERR) {
        fprintf(stderr, "[WARN] Could not install SIGINT handler\n");
    }

    if (signal(SIGTERM, bridge_runtime_signal_handler) == SIG_ERR) {
        fprintf(stderr, "[WARN] Could not install SIGTERM handler\n");
    }
}

#ifdef LHKT_TEST
void bridge_runtime_test_reset_hooks(void)
{
    runtime_test_sleep_call_count = 0;
}

void lhkt_test_bridge_reset_stop(void)
{
    bridge_runtime_reset_stop_requested();
}

void lhkt_test_bridge_request_stop(void)
{
    bridge_runtime_signal_handler(SIGTERM);
}

int lhkt_test_bridge_should_stop(void)
{
    return bridge_runtime_should_stop();
}
#endif

int bridge_runtime_add_fd_to_set(int fd, fd_set *set, int *max_fd)
{
    if (fd < 0 || !set || !max_fd) {
        return LHKT_ERR;
    }

    if (fd >= FD_SETSIZE) {
        return LHKT_ERR_LONG;
    }

    FD_SET(fd, set);

    if (fd > *max_fd) {
        *max_fd = fd;
    }

    return LHKT_OK;
}

#ifdef LHKT_TEST
int lhkt_test_add_fd_to_set(int fd)
{
    fd_set rfds;
    int max_fd;

    FD_ZERO(&rfds);
    max_fd = -1;

    return bridge_runtime_add_fd_to_set(fd, &rfds, &max_fd);
}

int lhkt_test_bridge_wait_fd_writable(int fd)
{
    bridge_rx_set_should_stop(bridge_runtime_should_stop);
    return bridge_rx_test_wait_fd_writable(fd);
}
#endif

int bridge_runtime_set_fd_nonblocking(int fd)
{
    int flags;

    if (fd < 0) {
        return LHKT_ERR;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return LHKT_ERR;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return LHKT_ERR;
    }

    return LHKT_OK;
}

#ifndef LHKT_TEST
static int runtime_sleep_ms_real(int ms)
{
    struct timeval tv;

    if (ms <= 0) {
        return LHKT_OK;
    }

    if (bridge_runtime_should_stop()) {
        return LHKT_ERR;
    }

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    while (select(0, NULL, NULL, NULL, &tv) < 0 && errno == EINTR) {
        if (bridge_runtime_should_stop()) {
            return LHKT_ERR;
        }

        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
    }

    if (bridge_runtime_should_stop()) {
        return LHKT_ERR;
    }

    return LHKT_OK;
}
#endif

int bridge_runtime_sleep_ms(int ms)
{
#ifdef LHKT_TEST
    if (ms > 0) {
        runtime_test_sleep_call_count++;
    }

    if (bridge_runtime_should_stop()) {
        return LHKT_ERR;
    }

    return LHKT_OK;
#else
    return runtime_sleep_ms_real(ms);
#endif
}

#ifdef LHKT_TEST
int lhkt_test_bridge_sleep_ms(int ms)
{
    return bridge_runtime_sleep_ms(ms);
}

size_t lhkt_test_bridge_sleep_call_count(void)
{
    return runtime_test_sleep_call_count;
}
#endif

long bridge_runtime_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}
