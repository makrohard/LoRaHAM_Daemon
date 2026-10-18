#include "daemon_ch341_runtime.h"

#include <mutex>
#include <string>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "ch341_hal.h"
#include "ch341_loader.h"
#include "daemon_radio_init.h"
#include "daemon_radio_runtime.h"
#include "config_apply.h"
#include "daemon_band.h"
#include "daemon_ch341_select.h"
#include "daemon_high_power_boot.h"
#include "daemon_rx_rearm.h"
#include "daemon_usb_wait.h"
#include "hardware_profile.h"
#include "loraham_runtime.h"
#include "radio_controller.h"

extern RadioController radio_controller;

static loraham_ch341_mon *g_mon = nullptr;
/* The event-loop owner of the monitor's fds; each registration is keyed by
 * its fd number (the generation argument), so an fd that stays in libusb's
 * set keeps its registration whatever else was added or removed. */
static char g_fd_owner;
static const LorahamCh341Api *g_api = nullptr;
static loraham_ch341 *g_selected = nullptr;
static const char *g_usb_serial = nullptr;
/* The configuration to replay after a replug, copied at teardown. */
static std::string g_replay;

/* ---- the state machine's operations ---------------------------------------- */

static uint64_t op_now_ms(void);

/* ---- selection operations ------------------------------------------------------ */

static int sel_scan_begin(void) { return g_api->mon_scan_begin(g_mon); }
static void sel_scan_step(int i, loraham_ch341_scan_item *out) { g_api->mon_scan_step(g_mon, i, out); }
static void sel_scan_end(void) { g_api->mon_scan_end(g_mon); }
static loraham_ch341 *sel_open(const char *serial)
{
    return g_api->open((uint16_t)daemon_hw_profile.usb_vid, (uint16_t)daemon_hw_profile.usb_pid,
                       serial);
}
static void sel_close(loraham_ch341 *dev) { g_api->close(dev); }
static int sel_identity(loraham_ch341 *dev, loraham_ch341_scan_item *out) { return g_api->identity(dev, out); }
static void sel_seed_idle(void) { g_api->seed_idle(); }
static int sel_bind(loraham_ch341 *dev) { return g_api->mon_bind(g_mon, dev); }
static void sel_unbind(void) { g_api->mon_unbind(g_mon); }
/* A selection does not commit on a LEFT or on lost events (2): hard-stale. */
static bool sel_bound_left(void) { return g_api->mon_left_bound(g_mon) != 0; }

/* Every ARRIVED and unbound LEFT the monitor has, to the selection (at start
 * and after each blocking open). */
static void sel_take_events(void)
{
    g_api->mon_dispatch(g_mon);
    while (g_api->mon_take_arrival(g_mon))
        daemon_ch341_select_note_arrival();
    while (g_api->mon_take_left(g_mon))
        daemon_ch341_select_note_left();
}

static void op_reopen_begin(bool) { daemon_ch341_select_begin(false); }
static void op_reopen_arrival(void) { daemon_ch341_select_note_arrival(); }
static void op_reopen_left(void) { daemon_ch341_select_note_left(); }

static int op_reopen_step(void)
{
    loraham_ch341 *dev = nullptr;
    char why[240];

    switch (daemon_ch341_select_step(&dev, why, sizeof(why))) {
    case CH341_SEL_PENDING:
        return 0;
    case CH341_SEL_OPENED:
        g_selected = dev;
        return 1;
    default:
        return 2;
    }
}

static bool op_reopen_due(void)
{
    uint64_t at = daemon_ch341_select_retry_at();
    return at != 0 && op_now_ms() >= at;
}

static void op_dispatch(void *) { g_api->mon_dispatch(g_mon); }
/* The runtime acts on a real LEFT only; lost events (2) leave the §3.3
 * "otherwise" path to the presence check (the two looks). */
static bool op_left_bound(void *) { return g_api->mon_left_bound(g_mon) == 1; }
static int op_present_bound(void *) { return g_api->mon_present_bound(g_mon); }
static bool op_take_arrival(void *) { return g_api->mon_take_arrival(g_mon) != 0; }
static bool op_take_left(void *) { return g_api->mon_take_left(g_mon) != 0; }

/* The radio part of radio_controller_shutdown(), without touching the stick
 * (it is gone: no standby, no IRQ clear) and keeping health UNPLUGGED and the
 * STATS counters. Never blocks: the TX worker may hold radio_mutex across a
 * transmit; its call into the refusing HAL ends on RadioLib's timeouts. */
static bool op_teardown_try(void)
{
    std::unique_lock<std::recursive_mutex> lk(radio_controller.radio_mutex, std::try_to_lock);

    if (!lk.owns_lock())
        return false;
    g_replay = config_apply_replay_command();
    g_api->mon_unbind(g_mon);
    radio_controller.driver.reset();
    radio_controller.mod.reset();
    radio_controller.hal_ctl = nullptr;
    radio_controller.hal.reset();          /* closes the device: pine64 deinit */
    radio_controller.received.store(false);
    radio_controller.rx_rearm_pending.store(false);
    radio_controller.cad_active.store(false);
    radio_controller.getrssi_active.store(false);
    snprintf(radio_controller.usb_wait_port, sizeof(radio_controller.usb_wait_port), "%s",
             daemon_ch341_select_wait_port());
    snprintf(radio_controller.bound, sizeof(radio_controller.bound), "-");
    return true;
}

static int op_reinit(void)
{
    /* Never wait on the main loop: busy is retried on the next pass, with the
     * selected stick kept open. */
    std::unique_lock<std::recursive_mutex> lk(radio_controller.radio_mutex, std::try_to_lock);
    if (!lk.owns_lock())
        return -1;

    /* The stick comes back with the band's boot defaults (LoRa). */
    radio_controller.mode = RADIO_MODE_LORA;
    config_apply_effective_reset();
    lora_init();
    if (!radio_controller_ready(&radio_controller))
        return 0;

    /* Replay what the clients had applied, through the same apply path (same
     * validation, the same 21/22 dBm warning; the OCP follows begin()/beginFSK()
     * inside the driver), then re-arm RX as CONFIG apply does. */
    if (!g_replay.empty()) {
        ConfigApplyStatus st = parse_and_apply_config_generic(
            *radio_controller.driver, daemon_band()->tag, g_replay.c_str(),
            radio_controller.mode, radio_controller.getrssi_active,
            daemon_high_power_enabled());
        if (st == CONFIG_APPLY_APPLIED) {
            radio_controller.driver->setPacketReceivedAction(radio_controller.rx_callback);
            daemon_rx_rearm_note_result(&radio_controller,
                                        radio_controller.driver->startReceive(), "REPLAY");
        } else if (st != CONFIG_APPLY_OK_NO_RADIO) {
            printf("[CH341] replay of the applied configuration failed: %s\n", g_replay.c_str());
            return 0;
        }
        printf("[CH341] configuration replayed: %s\n", g_replay.c_str());
    }
    snprintf(radio_controller.usb_wait_port, sizeof(radio_controller.usb_wait_port), "-");
    snprintf(radio_controller.bound, sizeof(radio_controller.bound), "%s", daemon_ch341_select_bound());
    return 1;
}

static void op_set_health(RadioHealth h) { radio_controller.health.store(h); }

/* Every exit 5 of the Meshstick: the device closed first (the HAL), then the
 * monitor (its references and libusb context), then _exit. */
static void close_monitor_for_exit(void)
{
    if (g_mon) {
        g_api->mon_close(g_mon);
        g_mon = nullptr;
    }
}

static void op_fatal(const char *why)
{
    if (radio_controller.hal_ctl)
        radio_controller.hal_ctl->runtime_fatal(why);   /* the stick, then the monitor, then _exit */
    if (g_selected) {                                   /* torn down: no HAL, maybe a selected stick */
        g_api->close(g_selected);
        g_selected = nullptr;
    }
    close_monitor_for_exit();
    fprintf(stderr, "[RADIO] FATAL: %s - aborting to prevent unsynchronised radio I/O\n", why);
    fflush(stderr);
    _exit(LORAHAM_EXIT_RUNTIME_RADIO_IO_ERROR);
}

static uint64_t op_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ---- public ------------------------------------------------------------------ */

void daemon_ch341_monitor_start(void)
{
    g_api = daemon_ch341_api();
    if (!g_api)
        return;
    g_mon = g_api->mon_open((uint16_t)daemon_hw_profile.usb_vid,
                            (uint16_t)daemon_hw_profile.usb_pid);
    if (!g_mon) {
        printf("[CH341] note: libusb offers no usable hotplug here; a lost stick "
               "exits 5 as before\n");
        return;
    }

    DaemonUsbWaitOps ops{};
    ops.mon = g_mon;
    ops.mon_dispatch = op_dispatch;
    ops.mon_left_bound = op_left_bound;
    ops.mon_present_bound = op_present_bound;
    ops.mon_take_arrival = op_take_arrival;
    ops.mon_take_left = op_take_left;
    ops.reopen_begin = op_reopen_begin;
    ops.reopen_arrival = op_reopen_arrival;
    ops.reopen_left = op_reopen_left;
    ops.reopen_step = op_reopen_step;
    ops.reopen_due = op_reopen_due;
    ops.teardown_try = op_teardown_try;
    ops.reinit = op_reinit;
    ops.set_health = op_set_health;
    ops.fatal = op_fatal;
    ops.now_ms = op_now_ms;
    daemon_usb_wait_init(&ops);
    printf("[CH341] unplug-wait active: a lost stick is waited for (libusb hotplug)\n");
}

bool daemon_ch341_monitor_active(void)
{
    return g_mon != nullptr;
}

void daemon_ch341_arm_hal(DaemonHalControl *ctl)
{
    if (g_mon && ctl)
        ctl->set_loss_wait(daemon_usb_wait_on_loss);
    if (g_mon && ctl)
        ctl->set_before_exit(close_monitor_for_exit);
}

static bool refuse(char *why, size_t why_size, const char *text)
{
    snprintf(why, why_size, "%s", text);
    return false;
}

bool daemon_ch341_select_at_start(const char *usb_serial, char *why, size_t why_size)
{
    if (!g_api)
        return true;
    if (!g_mon) {
        /* No monitor: no scan and no Rule B; pine64's own choice, checked. */
        loraham_ch341 *dev = sel_open(usb_serial);
        loraham_ch341_scan_item id;
        if (!dev)
            return refuse(why, why_size, "no Meshstick could be opened (not plugged in, no "
                          "permission, or in use by another process)");
        /* One identity read, the same rules as the post-open checks: an
         * unreadable serial is never "no serial" (DELTA-4-v3, every mode). */
        if (g_api->identity(dev, &id) != 0) {
            g_api->close(dev);
            return refuse(why, why_size, "the opened stick could not be identified (serial or port unreadable)");
        }
        if (id.serial_state == -1) {
            g_api->close(dev);
            return refuse(why, why_size, "the opened stick's serial could not be read (unreadable serial)");
        }
        if (usb_serial && usb_serial[0] &&
            !(id.serial_state == 1 && strncmp(id.serial, usb_serial, 8) == 0)) {
            g_api->close(dev);
            return refuse(why, why_size, "the opened stick is not the one --usb-serial names");
        }
        daemon_ch341_bound_text(&id, radio_controller.bound, sizeof(radio_controller.bound));
        g_selected = dev;
        return true;
    }

    DaemonCh341SelectOps ops{};
    ops.scan_begin = sel_scan_begin;
    ops.scan_step = sel_scan_step;
    ops.scan_end = sel_scan_end;
    ops.open = sel_open;
    ops.close = sel_close;
    ops.identity = sel_identity;
    ops.seed_idle = sel_seed_idle;
    ops.bind = sel_bind;
    ops.unbind = sel_unbind;
    ops.bound_left = sel_bound_left;
    ops.now_ms = op_now_ms;
    ops.take_events = sel_take_events;
    daemon_ch341_select_init(&ops, usb_serial);

    /* At start the steps run back to back (no clients yet); an ACCESS or an
     * inconclusive scan is retried until its 2 s deadline. */
    daemon_ch341_select_begin(true);
    for (;;) {
        /* A stick arriving during the start selection reaches it too: a
         * duplicate serial or a second stick makes it classify again. */
        sel_take_events();
        loraham_ch341 *dev = nullptr;
        DaemonCh341SelectResult r = daemon_ch341_select_step(&dev, why, why_size);
        if (r == CH341_SEL_OPENED) {
            g_selected = dev;
            snprintf(radio_controller.bound, sizeof(radio_controller.bound), "%s",
                     daemon_ch341_select_bound());
            return true;
        }
        if (r == CH341_SEL_REFUSED)
            return false;
        if (r == CH341_SEL_WAIT) {
            uint64_t at = daemon_ch341_select_retry_at();
            uint64_t now = op_now_ms();
            if (at > now)
                usleep((useconds_t)((at - now) * 1000u));
            daemon_ch341_select_begin(true);
        }
    }
}

void daemon_ch341_set_usb_serial(const char *serial)
{
    g_usb_serial = serial;
}

bool daemon_ch341_start(char *why, size_t why_size)
{
    daemon_ch341_monitor_start();
    return daemon_ch341_select_at_start(g_usb_serial, why, why_size);
}

void daemon_ch341_shutdown(void)
{
    if (!g_api)
        return;
    /* After the last HAL is gone: disarm the state machine, close a stick
     * selected but never handed to a HAL, then the monitor (its binding,
     * queued references and libusb context). */
    daemon_usb_wait_reset();
    if (g_mon)
        daemon_ch341_select_cancel();       /* a Rule B walk may hold sticks */
    if (g_selected) {
        g_api->close(g_selected);
        g_selected = nullptr;
    }
    if (g_mon) {
        g_api->mon_close(g_mon);
        g_mon = nullptr;
    }
}

loraham_ch341 *daemon_ch341_take_selected(void)
{
    loraham_ch341 *dev = g_selected;
    g_selected = nullptr;
    return dev;
}

void daemon_ch341_reconcile_fds(EventLoopSet *set)
{
    loraham_ch341_pollfd fds[16];
    int n;

    if (!g_mon)
        return;
    n = g_api->mon_fds(g_mon, fds, 16);
    if (n < 0) {
        /* Not registered in epoll: events are still handled by the per-pass
         * dispatch (daemon_usb_wait_tick), only woken by the 10 ms loop. */
        static bool warned = false;
        if (!warned) {
            printf("[CH341] warning: the monitor's pollfds could not be read (or more than 16); "
                   "hotplug events are handled on every loop pass instead\n");
            warned = true;
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        uint32_t ev = 0;
        if (fds[i].events & 0x001)       /* POLLIN */
            ev |= EVENT_LOOP_EVENT_READ;
        if (fds[i].events & 0x004)       /* POLLOUT */
            ev |= EVENT_LOOP_EVENT_WRITE;
        /* Not the notifier generation: libusb bumps it on every open/close on
         * the monitor context (each scan step), and a new generation for an
         * fd still registered would ADD it again (EEXIST, a terminal loop
         * error). An fd libusb dropped is not reconciled and is removed. */
        event_loop_reconcile_fd(set, &g_fd_owner, (uint32_t)fds[i].fd, fds[i].fd, ev);
    }
}
