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
/* --radio-error retry */
static bool g_fault_teardown = false; /* this teardown was decided by a fault: read once, cleared there */
static bool g_retrying = false;       /* a fault teardown ran: selections wait for the backoff time */
static bool g_kept = false;           /* the library kept a handle it refused to close: no new open */
static unsigned g_step = 0;           /* backoff delays used; carries across short READY periods */
static uint64_t g_backoff_at = 0;     /* no selection before this time while retrying */
static uint64_t g_ready_ms = 0;       /* READY since (the 10 min step reset) */
static unsigned g_attempts = 0;       /* selections since the loss that began this wait */

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
    g_fault_teardown = false;
    g_retrying = false;
    g_kept = false;
    g_step = 0;
    g_backoff_at = 0;
    g_ready_ms = 0;
    g_attempts = 0;
}

static const char *stick(void) { return g_ops.stick ? g_ops.stick : "USB stick"; }

/* exit 5 with the stick's model in the reason (fatal does not return in the daemon) */
static void fatal_stick(const char *before, const char *after)
{
    static char why[160];
    snprintf(why, sizeof(why), "%s%s%s", before, stick(), after);
    g_ops.fatal(why);
}

/* The next backoff delay (not yet used). */
static uint64_t backoff_ms(void)
{
    return g_step < DAEMON_USB_WAIT_BACKOFF_STEPS ? 1000ull << g_step : DAEMON_USB_WAIT_BACKOFF_CAP_MS;
}

/* Use it: no selection before now + the delay; the next one is longer. */
static uint64_t backoff_next(uint64_t now)
{
    uint64_t d = backoff_ms();
    g_backoff_at = now + d;
    if (g_step < DAEMON_USB_WAIT_BACKOFF_STEPS)
        g_step++;
    return d;
}

/* A selection without an event: when the selection's own retry is due; while
 * retrying, also not before the backoff time (its retry time 0 = the backoff
 * time). */
static bool reopen_due(uint64_t now)
{
    uint64_t at = g_ops.reopen_retry_at();
    if (!g_retrying)
        return at != 0 && now >= at;
    return now >= (at > g_backoff_at ? at : g_backoff_at);
}

static void usb_wait_gone(const char *signal)
{
    printf("[CH341] %s gone (%s): RADIO=UNPLUGGED, sockets stay, waiting for the stick\n",
           stick(), signal);
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
            if (g_step && g_ops.now_ms() - g_ready_ms >= DAEMON_USB_WAIT_STABLE_MS)
                g_step = 0;         /* READY held 10 min: the backoff starts at 1 s again */
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
        if (!g_retrying)
            g_attempts = 0;         /* a new loss: its selections are counted from 1 */
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
        if (present < 0) {
            fatal_stick("", " loss: the USB device list could not be read");
            return;
        }
        if (g_ops.retry) {
            uint32_t n = g_ops.count_fault();
            printf("[CH341] %s: USB I/O error with the stick present: closing it, retry in %llu s (fault %u)\n",
                   stick(), (unsigned long long)(backoff_ms() / 1000u), (unsigned)n);
            fflush(stdout);
            /* As for a LEFT (a failed CONF apply may have set FAILED meanwhile). */
            g_ops.set_health(RADIO_HEALTH_UNPLUGGED);
            g_fault_teardown = true;
            g_state = USB_WAIT_TEARDOWN;
            break;
        }
        fatal_stick("USB I/O error with the ", " present");
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
        {
            /* A refused close (retry mode) waits like a fault: no new
             * handle while the library keeps the old one. */
            bool kept = g_ops.retry && g_ops.handle_kept && g_ops.handle_kept();
            bool fault = g_fault_teardown || kept;
            g_fault_teardown = false;     /* one teardown's classification only */
            g_kept = kept;
            /* After a plain loss one selection right away: the stick may
             * already be back (its ARRIVED may have come before the loss was
             * noticed). After a fault: the backoff first. */
            g_force_reopen = !fault;
            if (fault) {
                g_retrying = true;
                backoff_next(g_ops.now_ms());
            }
        }
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
            if (g_kept) {
                /* Never a new handle: each due attempt only says so. */
                uint64_t now = g_ops.now_ms();
                if (now < g_backoff_at)
                    return;
                g_attempts++;
                uint64_t d = backoff_next(now);
                printf("[CH341] %s: attempt %u: the library kept the old handle: restart the daemon "
                       "(next attempt in %llu s)\n", stick(), g_attempts, (unsigned long long)(d / 1000u));
                fflush(stdout);
                return;
            }
            if (!g_reopening) {
                if (!arrival && !g_force_reopen && !reopen_due(g_ops.now_ms()))
                    return;
                g_force_reopen = false;
                g_attempts++;
                g_ops.reopen_begin(arrival);
                g_reopening = true;
            }
            int r = g_ops.reopen_step();
            if (r == 0)
                return;             /* next step on the next pass */
            g_reopening = false;
            if (r == 2) {
                /* not this time: wait for an ARRIVED or the retry */
                if (g_retrying) {
                    uint64_t d = backoff_next(g_ops.now_ms());
                    printf("[CH341] %s: attempt %u: not opened, next attempt in %llu s\n", stick(),
                           g_attempts, (unsigned long long)(d / 1000u));
                    fflush(stdout);
                }
                return;
            }
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
            printf("[CH341] %s lost again during the re-init: classifying\n", stick());
            fflush(stdout);
            g_state = USB_WAIT_READY;
            return;
        }
        if (ri == 1) {
            if (g_retrying)
                printf("[CH341] %s back after %u attempts: RADIO=READY\n", stick(), g_attempts);
            else
                printf("[CH341] %s back: RADIO=READY\n", stick());
            fflush(stdout);
            g_retrying = false;
            g_ready_ms = g_ops.now_ms();
            g_state = USB_WAIT_READY;
            return;
        }
        if (g_ops.retry) {
            /* Closed again; the next selection waits for the next step. */
            printf("[CH341] %s: attempt %u: re-init failed: closing it, next attempt in %llu s\n", stick(),
                   g_attempts, (unsigned long long)(backoff_ms() / 1000u));
            fflush(stdout);
            g_ops.set_health(RADIO_HEALTH_UNPLUGGED);
            g_fault_teardown = true;
            g_state = USB_WAIT_TEARDOWN;
            return;
        }
        g_ops.set_health(RADIO_HEALTH_FAILED);
        fatal_stick("", " re-init after the replug failed");
        return;
    }
    }
}
