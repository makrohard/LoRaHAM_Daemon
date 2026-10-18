#ifndef LORAHAM_DAEMON_CH341_SELECT_H
#define LORAHAM_DAEMON_CH341_SELECT_H

#include <stddef.h>
#include <stdint.h>

#include "ch341_adapter.h"

/* --- Which Meshstick: selection at start and at every reopen ------------------ */
/*
 * pine64 opens the first free 1A86:5512 (or, with a serial, the first whose 8
 * serial characters match). The rules on top (plan v10 §3.2):
 *   --usb-serial S        exactly one attached stick with S, no "unknown"
 *                         stick beside it; opened by S, checked after the open.
 *   no selector, 1 stick  plain open; its identity is remembered (the serial if
 *                         usable, else the port path).
 *   no selector, several  exactly one stick without a serial -> Rule B: open
 *                         free sticks one by one, hold those with a serial,
 *                         until the one without opens; release the held ones.
 *                         More than one without a serial, or none: refused.
 *   reopen                by the remembered serial (any port), or by Rule B
 *                         and the remembered port for the stick without one.
 * Every open passes the post-open checks before any radio init. A stick that
 * is present and openable but that pine64 could not take is BUSY: at start
 * exit 4 with the "[CH341] refused: BUSY:" line; on reopen keep waiting and
 * retry. ACCESS counts from its first observation, 2 s. On a reopen a Rule B
 * walk settles on the ARRIVED burst (500 ms quiet, at most 2 s).
 *
 * One bounded step per call (one scan step or one open attempt), so the main
 * loop serves the sockets between steps. At start the caller loops.
 */

typedef struct {
    int (*scan_begin)(void);
    void (*scan_step)(int i, loraham_ch341_scan_item *out);
    void (*scan_end)(void);
    loraham_ch341 *(*open)(const char *serial);       /* NULL: pine64 took none */
    void (*close)(loraham_ch341 *dev);
    int (*identity)(loraham_ch341 *dev, loraham_ch341_scan_item *out);
    void (*seed_idle)(void);
    int (*bind)(loraham_ch341 *dev);                  /* 0 ok; NULL op = no monitor */
    void (*unbind)(void);                             /* undo bind (the result is not committed) */
    bool (*bound_left)(void);                         /* the bound stick's LEFT seen since the bind */
    uint64_t (*now_ms)(void);
    /* Dispatch the monitor and feed every ARRIVED to
     * daemon_ch341_select_note_arrival() and every unbound LEFT to
     * daemon_ch341_select_note_left(): called after each blocking open
     * (also a failed one) and identity read, before a result is committed.
     * NULL op = no monitor. */
    void (*take_events)(void);
} DaemonCh341SelectOps;

typedef enum {
    CH341_SEL_PENDING = 0,   /* call again (next pass) */
    CH341_SEL_OPENED,        /* *out is the checked, bound stick */
    CH341_SEL_WAIT,          /* nothing to open now; retry at the due time or an ARRIVED */
    CH341_SEL_REFUSED        /* at start: exit 4 with `why` */
} DaemonCh341SelectResult;

#define DAEMON_CH341_ACCESS_MS 2000u
#define DAEMON_CH341_BUSY_RETRY_MS 200u
#define DAEMON_CH341_WALK_GAP_MS 1000u
#define DAEMON_CH341_SETTLE_QUIET_MS 500u
#define DAEMON_CH341_SETTLE_CAP_MS 2000u

void daemon_ch341_select_init(const DaemonCh341SelectOps *ops, const char *usb_serial);

/* Start a selection (at_start: refusals are final). */
void daemon_ch341_select_begin(bool at_start);

/* An ARRIVED taken while UNPLUGGED (also during a selection): a Rule B walk
 * settles on the burst, and an ARRIVED during a scan makes it scan again. */
void daemon_ch341_select_note_arrival(void);

/* A LEFT of a stick other than the bound one (taken from the monitor): the
 * snapshot no longer matches the bus, so a pending result is classified
 * again. No settle burst (that counts ARRIVEDs only). */
void daemon_ch341_select_note_left(void);

/* Shutdown: release every stick a walk holds and end any scan. */
void daemon_ch341_select_cancel(void);
DaemonCh341SelectResult daemon_ch341_select_step(loraham_ch341 **out, char *why, size_t why_size);

/* After CH341_SEL_WAIT: when a retry is due without an ARRIVED (BUSY, ACCESS,
 * inconclusive); 0 = only an ARRIVED retries. */
uint64_t daemon_ch341_select_retry_at(void);

/* The port the daemon of a stick without a serial waits on, or "-". */
const char *daemon_ch341_select_wait_port(void);

/* The identity of the last verified open, for STATUS BOUND=:
 * "serial:<first 8 characters>", or "port:<bus-port>" only for a stick with
 * iSerialNumber 0 (an unreadable serial is never port:); bytes outside
 * 0x21..0x7E and '%' are written as %XX. "-" before the first open. */
const char *daemon_ch341_select_bound(void);
void daemon_ch341_bound_text(const loraham_ch341_scan_item *id, char *out, size_t n);

/* Tests only. */
void daemon_ch341_select_reset(void);

#endif
