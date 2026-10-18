#ifndef LORAHAM_DAEMON_USB_WAIT_H
#define LORAHAM_DAEMON_USB_WAIT_H

#include <stdint.h>

#include "radio_health.h"

/* --- Unplug-wait: the Meshstick daemon waits for its stick ----------------- */
/*
 * After READY, a failed USB call latches a loss in the HAL (daemon_hal_control.h)
 * and on_loss() runs: RADIO=UNPLUGGED at once, so every admission check refuses
 * TX and CONF SET before any mutex. The main loop then classifies within the
 * 500 ms grace: a LEFT for the bound device, or at the deadline the presence
 * check, decides "gone" (tear the radio down, keep every socket, wait) or "I/O
 * fault with the stick present" (exit 5, as before). A returning stick is
 * re-initialised as at boot and the applied configuration replayed; a failed
 * re-init is RADIO=FAILED and exit 5.
 *
 * Everything here runs on the main loop except daemon_usb_wait_on_loss().
 * The monitor (the adapter's own libusb context) and the daemon's radio steps
 * come in through DaemonUsbWaitOps, so the state machine is tested on fakes.
 */

typedef enum {
    USB_WAIT_READY = 0,     /* no loss latched */
    USB_WAIT_PENDING,       /* loss latched, being classified (grace) */
    USB_WAIT_TEARDOWN,      /* gone; radio teardown waits for radio_mutex */
    USB_WAIT_UNPLUGGED      /* torn down; waiting for the stick */
} DaemonUsbWaitState;

typedef struct {
    /* The monitor. */
    void *mon;
    void (*mon_dispatch)(void *mon);           /* handle pending hotplug events */
    bool (*mon_left_bound)(void *mon);          /* a LEFT for the bound device seen */
    int  (*mon_present_bound)(void *mon);       /* 1 present, 0 gone, -1 list read failed */
    bool (*mon_take_arrival)(void *mon);        /* an ARRIVED waits to be acted on (consumed) */
    bool (*mon_take_left)(void *mon);           /* a LEFT of another stick, likewise (NULL = none) */

    /* The reopen (daemon_ch341_select.h): begun by an ARRIVED (arrival = true)
     * or a due retry, then one bounded step per pass. */
    void (*reopen_begin)(bool arrival);
    void (*reopen_arrival)(void);               /* every ARRIVED taken while UNPLUGGED */
    void (*reopen_left)(void);                  /* every unbound LEFT taken while UNPLUGGED */
    int  (*reopen_step)(void);                  /* 0 pending, 1 opened, 2 wait */
    bool (*reopen_due)(void);                   /* a retry is due without an ARRIVED */

    /* The daemon's radio. */
    bool (*teardown_try)(void);                 /* false = radio_mutex busy, retry */
    int  (*reinit)(void);                       /* re-init on the opened stick + replay:
                                                   1 READY, 0 failed, -1 radio_mutex busy (retry) */
    void (*set_health)(RadioHealth health);
    void (*fatal)(const char *why);             /* exit 5; does not return in the daemon */

    uint64_t (*now_ms)(void);                   /* monotonic */
} DaemonUsbWaitOps;

#define DAEMON_USB_WAIT_GRACE_MS 500u

/* Arm the state machine with its operations (main loop, before READY). */
void daemon_usb_wait_init(const DaemonUsbWaitOps *ops);

/* The HAL's on_loss hook: any thread (atomics only). The first call also
 * stamps the loss time; the grace deadline is counted from it. */
void daemon_usb_wait_on_loss(void);

/* One step, every main-loop pass. */
void daemon_usb_wait_tick(void);

DaemonUsbWaitState daemon_usb_wait_state(void);

/* Disarm (shutdown, tests): no further tick acts. */
void daemon_usb_wait_reset(void);

#endif
