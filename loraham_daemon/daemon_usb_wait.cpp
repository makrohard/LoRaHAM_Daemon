#include "daemon_usb_wait.h"

#include <atomic>
#include <stdio.h>

static DaemonUsbWaitOps g_ops;
static bool g_armed = false;
static std::atomic<bool> g_loss_flag{false};
static DaemonUsbWaitState g_state = USB_WAIT_READY;
static uint64_t g_deadline_ms = 0;
static bool g_reopening = false;
static bool g_force_reopen = false;  /* one selection right after the teardown */
static bool g_reinit_pending = false; /* a stick is open, re-init waits for radio_mutex */
static std::atomic<uint64_t> g_loss_ms{0};

void daemon_usb_wait_init(const DaemonUsbWaitOps *ops)
{
    g_ops = *ops;
    g_armed = true;
}

void daemon_usb_wait_on_loss(void)
{
    /* Admission first: every readiness check refuses from this store on. */
    if (g_armed && g_ops.set_health)
        g_ops.set_health(RADIO_HEALTH_UNPLUGGED);
    uint64_t expected = 0;
    if (g_armed && g_ops.now_ms)
        g_loss_ms.compare_exchange_strong(expected, g_ops.now_ms());
    g_loss_flag.store(true);
}

DaemonUsbWaitState daemon_usb_wait_state(void)
{
    return g_state;
}

void daemon_usb_wait_reset(void)
{
    g_armed = false;
    g_loss_flag.store(false);
    g_state = USB_WAIT_READY;
    g_deadline_ms = 0;
    g_reopening = false;
    g_force_reopen = false;
    g_reinit_pending = false;
    g_loss_ms.store(0);
}

static void usb_wait_gone(const char *signal)
{
    printf("[CH341] Meshstick gone (%s): RADIO=UNPLUGGED, sockets stay, waiting for the stick\n",
           signal);
    fflush(stdout);
    g_state = USB_WAIT_TEARDOWN;
}

void daemon_usb_wait_tick(void)
{
    if (!g_armed)
        return;

    /* Every pass: the monitor's fds are level-triggered in the main loop's
     * epoll set, so pending events are always handled (never left to spin). */
    g_ops.mon_dispatch(g_ops.mon);

    switch (g_state) {
    case USB_WAIT_READY:
        if (!g_loss_flag.load()) {
            /* An ARRIVED while the stick is up is another stick, unless the
             * bound one already left: a fast replug can deliver LEFT and
             * ARRIVED before the HAL notices. Those arrivals are kept. */
            if (!g_ops.mon_left_bound(g_ops.mon)) {
                while (g_ops.mon_take_arrival(g_ops.mon)) { }
                while (g_ops.mon_take_left && g_ops.mon_take_left(g_ops.mon)) { }
            }
            return;
        }
        /* The deadline counts from the first failure (stamped by on_loss);
         * a later failure never restarts it. */
        g_deadline_ms = g_loss_ms.load() + DAEMON_USB_WAIT_GRACE_MS;
        g_state = USB_WAIT_PENDING;
        /* fall through: a LEFT may already be queued */
        [[fallthrough]];

    case USB_WAIT_PENDING: {
        if (g_ops.mon_left_bound(g_ops.mon)) {
            usb_wait_gone("LEFT event; presence check not called");
            break;
        }
        if (g_ops.now_ms() < g_deadline_ms)
            return;
        int present = g_ops.mon_present_bound(g_ops.mon);
        if (present == 0) {
            usb_wait_gone("absent at the grace deadline");
            break;
        }
        if (present < 0)
            g_ops.fatal("Meshstick loss: the USB device list could not be read");
        else
            g_ops.fatal("USB I/O error with the Meshstick present");
        return;
    }

    case USB_WAIT_TEARDOWN:
        if (!g_ops.teardown_try())
            return;   /* radio_mutex held by the TX worker: next pass */
        /* The old HAL is gone: its loss is retired here, so a loss reported
         * from now on (the next HAL, armed inside the re-init) is a new one
         * with its own grace. */
        g_loss_flag.store(false);
        g_loss_ms.store(0);
        g_state = USB_WAIT_UNPLUGGED;
        /* One selection right away: the stick may already be back (its
         * ARRIVED may have come before the loss was noticed). */
        g_force_reopen = true;
        return;

    case USB_WAIT_UNPLUGGED: {
        if (!g_reinit_pending) {
            /* Every ARRIVED reaches the selection, also during a running
             * one (its Rule B settle and snapshot depend on it); so does every
             * LEFT of another stick (a duplicate gone makes the wanted one
             * unique: a refusal waiting for an event must see it). */
            bool arrival = false;
            while (g_ops.mon_take_arrival(g_ops.mon)) {
                arrival = true;
                if (g_ops.reopen_arrival)
                    g_ops.reopen_arrival();
            }
            while (g_ops.mon_take_left && g_ops.mon_take_left(g_ops.mon)) {
                arrival = true;
                if (g_ops.reopen_left)
                    g_ops.reopen_left();
            }
            if (!g_reopening) {
                if (!arrival && !g_force_reopen && !g_ops.reopen_due())
                    return;
                g_force_reopen = false;
                g_ops.reopen_begin(arrival);
                g_reopening = true;
            }
            int r = g_ops.reopen_step();
            if (r == 0)
                return;             /* next step on the next pass */
            g_reopening = false;
            if (r == 2)
                return;             /* not this time: wait for an ARRIVED or the retry */
            g_reinit_pending = true;
        }
        int ri = g_ops.reinit();
        if (ri < 0)
            return;                 /* radio_mutex busy: the stick stays open, next pass */
        g_reinit_pending = false;
        if (ri == 1 && g_loss_flag.load()) {
            /* The new HAL latched a loss during the re-init's replay or RX
             * re-arm (on_loss: UNPLUGGED, the flag, a new stamp). It never
             * reports again, so it is classified like any loss, never READY. */
            printf("[CH341] Meshstick lost again during the re-init: classifying\n");
            fflush(stdout);
            g_state = USB_WAIT_READY;
            return;
        }
        if (ri == 1) {
            printf("[CH341] Meshstick back: RADIO=READY\n");
            fflush(stdout);
            g_state = USB_WAIT_READY;
            return;
        }
        g_ops.set_health(RADIO_HEALTH_FAILED);
        g_ops.fatal("Meshstick re-init after the replug failed");
        return;
    }
    }
}
