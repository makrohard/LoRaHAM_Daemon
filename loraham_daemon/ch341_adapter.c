/* --- CH341 adapter: built into lib/libloraham-ch341.so, never into the daemon --- */
/*
 * Thin wrapper around pine64's libpinedio-usb (see ch341_adapter.h). No CH341
 * logic lives here: every call is the library's own. Two settings are made on
 * open, the ones meshtasticd's USBHal makes for the same stick: chip select is
 * left to the caller (RadioLib drives it as a pin), and D3/D5 (SPI clock and
 * data out) are marked as outputs -- the library writes its direction mask with
 * every pin write, so without them the first chip-select write would turn the
 * SPI lines into inputs.
 */

#include "ch341_adapter.h"

#include <stdlib.h>
#include <string.h>

#include "libpinedio-usb.h"

#ifndef LORAHAM_CH341_PIN
#error "build.sh defines LORAHAM_CH341_PIN as the measured library commit"
#endif

struct loraham_ch341 {
    struct pinedio_inst inst;
};

const char *loraham_ch341_pin(void)
{
    return LORAHAM_CH341_PIN;
}

loraham_ch341 *loraham_ch341_open(uint16_t vid, uint16_t pid, const char *serial)
{
    loraham_ch341 *dev = calloc(1, sizeof(*dev));

    if (!dev)
        return NULL;

    if (serial && serial[0] != '\0') {
        strncpy(dev->inst.serial_number, serial, 8);
        pinedio_set_option(&dev->inst, PINEDIO_OPTION_SEARCH_SERIAL, 1);
    }
    pinedio_set_option(&dev->inst, PINEDIO_OPTION_VID, vid);
    pinedio_set_option(&dev->inst, PINEDIO_OPTION_PID, pid);

    if (pinedio_init(&dev->inst, NULL) != 0) {
        free(dev);
        return NULL;
    }

    /* pinedio_set_option() has no return statement at the pinned commit
     * (libpinedio-usb.c:327-329): its result is not a value and is not read.
     * pinedio_set_pin_mode() returns 0 there (:343-347) but is checked, so a
     * library change cannot hand out a device whose SPI lines are inputs. */
    pinedio_set_option(&dev->inst, PINEDIO_OPTION_AUTO_CS, 0);
    if (pinedio_set_pin_mode(&dev->inst, 3, 1) < 0 ||
        pinedio_set_pin_mode(&dev->inst, 5, 1) < 0) {
        pinedio_deinit(&dev->inst);
        free(dev);
        return NULL;
    }
    return dev;
}

void loraham_ch341_close(loraham_ch341 *dev)
{
    if (!dev)
        return;
    pinedio_deinit(&dev->inst);
    free(dev);
}

int loraham_ch341_transceive(loraham_ch341 *dev, const uint8_t *out, uint8_t *in, uint32_t len)
{
    uint8_t *scratch = NULL;
    int ret;

    if (!in) {
        scratch = malloc(len ? len : 1);
        if (!scratch)
            return -1;
        in = scratch;
    }
    /* The library reads `out` only; its parameter is not const. */
    ret = pinedio_transceive(&dev->inst, (uint8_t *)out, in, len);
    free(scratch);
    return ret < 0 ? -1 : 0;
}

int loraham_ch341_pin_mode(loraham_ch341 *dev, uint32_t pin, int output)
{
    return pinedio_set_pin_mode(&dev->inst, pin, output ? 1 : 0) < 0 ? -1 : 0;
}

int loraham_ch341_digital_write(loraham_ch341 *dev, uint32_t pin, int level)
{
    return pinedio_digital_write(&dev->inst, pin, level != 0) < 0 ? -1 : 0;
}

int loraham_ch341_digital_read(loraham_ch341 *dev, uint32_t pin)
{
    int32_t ret = pinedio_digital_read(&dev->inst, pin);

    if (ret < 0)
        return -1;
    return ret != 0 ? 1 : 0;
}

int loraham_ch341_attach_interrupt(loraham_ch341 *dev, uint32_t pin, uint32_t mode,
                                   void (*callback)(void))
{
    if (pin >= PINEDIO_INT_PIN_MAX)
        return -1;
    return pinedio_attach_interrupt(&dev->inst, (enum pinedio_int_pin)pin,
                                    (enum pinedio_int_mode)mode, callback) != 0 ? -1 : 0;
}

int loraham_ch341_detach_interrupt(loraham_ch341 *dev, uint32_t pin)
{
    if (pin >= PINEDIO_INT_PIN_MAX)
        return -1;
    return pinedio_deattach_interrupt(&dev->inst, (enum pinedio_int_pin)pin) != 0 ? -1 : 0;
}

int loraham_ch341_in_error(loraham_ch341 *dev)
{
    return dev->inst.in_error ? 1 : 0;
}
