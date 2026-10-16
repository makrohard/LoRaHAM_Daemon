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
} LorahamCh341Api;

#ifdef __cplusplus
}
#endif

#endif
