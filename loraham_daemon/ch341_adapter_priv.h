#ifndef LORAHAM_CH341_ADAPTER_PRIV_H
#define LORAHAM_CH341_ADAPTER_PRIV_H

/* Shared by ch341_adapter.c and ch341_monitor.c only (inside the .so). */
#include <stddef.h>
#include "libpinedio-usb.h"

struct loraham_ch341 {
    struct pinedio_inst inst;
};

/* The serial of an open handle within one absolute budget: 1 read (first 8
 * characters into out), 0 no serial (index 0), -1 unknown. */
int loraham_ch341_read_serial(libusb_device_handle *h, uint8_t index, char out[9],
                              unsigned budget_ms);

/* "<bus>-<port>[.<port>...]" of a device. 0 ok, -1 failed. */
int loraham_ch341_port_path(libusb_device *d, char *out, size_t size);

#endif
