#ifndef LORAHAM_CH341_ADAPTER_H
#define LORAHAM_CH341_ADAPTER_H

/* --- CH341 adapter: the one interface between the daemon and libpinedio-usb --- */
/*
 * The ELECROW Meshstick is an SX1262 behind a CH341 USB-to-SPI bridge. The
 * daemon drives it through pine64's libch341-spi-userspace (libpinedio-usb),
 * pinned in .github/ci/libpinedio.lock and used unchanged. build.sh compiles
 * that library's source together with ch341_adapter.c into
 * lib/libloraham-ch341.so beside the daemon binary; the daemon opens the .so
 * at startup with dlopen() only for `--hw meshstick-1262`.
 *
 * This header is all the daemon sees of it. The library keeps its instance
 * struct (with libusb types) in its own header; the adapter owns that struct,
 * so the daemon needs no libusb headers and cannot misuse a struct layout from
 * a different library build. `loraham_ch341_pin()` returns the library commit
 * the .so was built from, measured from the source tree by build.sh; the
 * loader refuses a .so whose value differs from the daemon's own lock.
 *
 * Pins are the CH341's D0..D7 (and the library's ERR/PEMP/INT inputs for
 * interrupts). Every function returns < 0 on a USB failure.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loraham_ch341 loraham_ch341;

/* The library commit this .so was built from (40 hex characters). */
const char *loraham_ch341_pin(void);

/* Open the first CH341 with this VID/PID (and this 8-character serial when
 * not NULL), claim interface 0 (detaching the kernel's spi-ch341 driver), and
 * leave chip select to the caller. NULL when no device could be opened. */
loraham_ch341 *loraham_ch341_open(uint16_t vid, uint16_t pid, const char *serial);

/* Stop the interrupt poll thread, release interface 0, re-attach the kernel
 * driver, close the device. NULL is ignored. */
void loraham_ch341_close(loraham_ch341 *dev);

/* One full-duplex SPI transfer of `len` bytes; `in` may be NULL. */
int loraham_ch341_transceive(loraham_ch341 *dev, const uint8_t *out, uint8_t *in, uint32_t len);

/* output != 0: the pin drives; otherwise it is an input. */
int loraham_ch341_pin_mode(loraham_ch341 *dev, uint32_t pin, int output);
int loraham_ch341_digital_write(loraham_ch341 *dev, uint32_t pin, int level);
/* 0 or 1, or < 0 on failure. */
int loraham_ch341_digital_read(loraham_ch341 *dev, uint32_t pin);

/* mode: 1 = rising, 2 = falling (the library's pinedio_int_mode values).
 * The callback runs on the library's poll thread. */
int loraham_ch341_attach_interrupt(loraham_ch341 *dev, uint32_t pin, uint32_t mode,
                                   void (*callback)(void));
int loraham_ch341_detach_interrupt(loraham_ch341 *dev, uint32_t pin);

/* Non-zero once the library's poll thread lost the device. */
int loraham_ch341_in_error(loraham_ch341 *dev);

/* --- The monitor: the adapter's own libusb context (ch341_monitor.c) ------- */
/*
 * Unplug-wait needs to know whether the stick it had is gone, and when a stick
 * arrives. pine64 keeps its own (default) libusb context; the monitor opens a
 * second one with one hotplug callback for the VID/PID. The callback only
 * records; everything else runs from the daemon's main loop. Device pointers of
 * the two contexts are never compared: the opened stick is matched into the
 * monitor's list once, by bus, port path and device address, at the verified
 * open (bind), and from then on the monitor's own device object IS the
 * connection (Linux reuses device numbers; libusb makes a new object for a new
 * connection).
 */
typedef struct loraham_ch341_mon loraham_ch341_mon;

typedef struct {
    int fd;
    short events;
} loraham_ch341_pollfd;

/* One stick as a scan step saw it. */
typedef struct {
    int openable;        /* 1 opened and closed again, 0 ACCESS, -1 gone, -2 other error */
    int serial_state;    /* 1 serial read, 0 no serial (index 0), -1 unknown */
    char serial[9];      /* first 8 characters when serial_state == 1 */
    char port[32];       /* "<bus>-<port>[.<port>...]" */
    unsigned connection; /* the monitor's cookie for this connection (its libusb_device object): a replug gets a new one */
} loraham_ch341_scan_item;

/* NULL when libusb has no hotplug, when its pollfds do not cover its
 * timeouts, or on any init failure: the daemon then keeps the old behaviour. */
loraham_ch341_mon *loraham_ch341_mon_open(uint16_t vid, uint16_t pid);
void loraham_ch341_mon_close(loraham_ch341_mon *m);

/* The context's pollfds with libusb's event masks: the count, or -1 when max is
 * too small. The generation changes whenever libusb adds or removes one. */
int loraham_ch341_mon_fds(loraham_ch341_mon *m, loraham_ch341_pollfd *out, int max);
unsigned loraham_ch341_mon_fd_generation(loraham_ch341_mon *m);

/* Handle pending events without waiting, then fold the recorded ones: a LEFT
 * for the bound device sets the left flag, any other LEFT adds one left, an
 * ARRIVED adds one arrival. */
void loraham_ch341_mon_dispatch(loraham_ch341_mon *m);
int loraham_ch341_mon_left_bound(loraham_ch341_mon *m);   /* 1 the bound stick's LEFT seen; 2 events were
                                                              lost while bound (it may have left); 0 none;
                                                              until the next bind */
int loraham_ch341_mon_take_arrival(loraham_ch341_mon *m); /* 1 and consumes one, else 0 */
int loraham_ch341_mon_take_left(loraham_ch341_mon *m);    /* the same for an unbound LEFT */

/* Match the opened stick into the monitor's list (bus, port path, address);
 * 0 when exactly one matches, -1 otherwise. Replaces any earlier binding. */
int loraham_ch341_mon_bind(loraham_ch341_mon *m, loraham_ch341 *dev);
void loraham_ch341_mon_unbind(loraham_ch341_mon *m);

/* Is the bound connection still attached? 1 present, 0 gone, -1 the device
 * list could not be read. Two looks: the list; if the object is in it, drain
 * the events (a LEFT for it is decisive) and read the list once more. No
 * device is opened and no request is sent. */
int loraham_ch341_mon_present_bound(loraham_ch341_mon *m);

/* A scan, one stick per step: begin takes the list (the count, or -1), each
 * step opens and closes stick i and reads its serial within one 200 ms budget,
 * end releases the list. */
int loraham_ch341_mon_scan_begin(loraham_ch341_mon *m);
void loraham_ch341_mon_scan_step(loraham_ch341_mon *m, int i, loraham_ch341_scan_item *out);
void loraham_ch341_mon_scan_end(loraham_ch341_mon *m);

/* The opened stick's own identity: its serial (read by the adapter, never
 * pine64's buffer) and its port path. 0 ok, -1 failed. */
int loraham_ch341_identity(loraham_ch341 *dev, loraham_ch341_scan_item *out);

/* Before an open that may land on a stick another daemon owns (Rule B): set
 * pine64's process-wide pin state so its open keeps RESET (D2) an output, high.
 * Its open still clears the D0 (CS) state bit (pinedio_set_cs, libpinedio-usb.c:318,
 * :371-373); it does not make D0 an output. */
void loraham_ch341_seed_idle(void);

/* The functions above as the daemon's loader resolves them (and a test fakes
 * them). One table, filled completely or not at all. */
typedef struct {
    const char *(*pin)(void);
    loraham_ch341 *(*open)(uint16_t vid, uint16_t pid, const char *serial);
    void (*close)(loraham_ch341 *dev);
    int (*transceive)(loraham_ch341 *dev, const uint8_t *out, uint8_t *in, uint32_t len);
    int (*pin_mode)(loraham_ch341 *dev, uint32_t pin, int output);
    int (*digital_write)(loraham_ch341 *dev, uint32_t pin, int level);
    int (*digital_read)(loraham_ch341 *dev, uint32_t pin);
    int (*attach_interrupt)(loraham_ch341 *dev, uint32_t pin, uint32_t mode,
                            void (*callback)(void));
    int (*detach_interrupt)(loraham_ch341 *dev, uint32_t pin);
    int (*in_error)(loraham_ch341 *dev);
    loraham_ch341_mon *(*mon_open)(uint16_t vid, uint16_t pid);
    void (*mon_close)(loraham_ch341_mon *m);
    int (*mon_fds)(loraham_ch341_mon *m, loraham_ch341_pollfd *out, int max);
    unsigned (*mon_fd_generation)(loraham_ch341_mon *m);
    void (*mon_dispatch)(loraham_ch341_mon *m);
    int (*mon_left_bound)(loraham_ch341_mon *m);
    int (*mon_take_arrival)(loraham_ch341_mon *m);
    int (*mon_take_left)(loraham_ch341_mon *m);
    int (*mon_bind)(loraham_ch341_mon *m, loraham_ch341 *dev);
    void (*mon_unbind)(loraham_ch341_mon *m);
    int (*mon_present_bound)(loraham_ch341_mon *m);
    int (*mon_scan_begin)(loraham_ch341_mon *m);
    void (*mon_scan_step)(loraham_ch341_mon *m, int i, loraham_ch341_scan_item *out);
    void (*mon_scan_end)(loraham_ch341_mon *m);
    int (*identity)(loraham_ch341 *dev, loraham_ch341_scan_item *out);
    void (*seed_idle)(void);
} LorahamCh341Api;

#ifdef __cplusplus
}
#endif

#endif
