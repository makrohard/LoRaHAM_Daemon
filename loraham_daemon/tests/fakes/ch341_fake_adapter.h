#ifndef LORAHAM_TEST_FAKE_CH341_ADAPTER_H
#define LORAHAM_TEST_FAKE_CH341_ADAPTER_H

/*
 * A fake of the Meshstick adapter table (ch341_adapter.h) for Ch341Hal tests:
 * SPI goes to an Sx126xRegisterModel, so the REAL Sx1262Driver and the REAL
 * pinned RadioLib run over the REAL Ch341Hal with no USB and no library.
 *
 * It records what a test needs to see -- opens and closes, the last level
 * written per CH341 pin, the interrupt pin and callback -- and injects the two
 * failures Ch341Hal must handle: a failing SPI transfer and the library's
 * in_error flag (the poll thread lost the device). `close_log`, when set,
 * gets one line per close() through write(), so a forked child's closes
 * are visible to the parent after the child has _exit()ed.
 */

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "../../ch341_adapter.h"
#include "sx126x_register_model.h"

struct FakeCh341 {
    Sx126xRegisterModel *model = nullptr;
    bool open_ok = true;
    int opens = 0;
    int closes = 0;
    bool fail_transceive = false;
    int transceives = 0;                                   /* calls that reached the device */
    bool in_error = false;

    int level[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };   /* last write, -1 = never */
    int writes_high[8] = { 0 };
    int writes_low[8] = { 0 };
    int mode[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    int read_raw[8] = { 0 };                               /* what digital_read() sees */
    int spi_at_reset_done = -1;    /* transfers before RESET (D2) first went LOW then HIGH */

    uint32_t irq_pin = UINT32_MAX;
    void (*irq_cb)(void) = nullptr;

    const char *close_log = nullptr;
};

static FakeCh341 g_fake_ch341;

static void fake_ch341_reset(Sx126xRegisterModel *model)
{
    g_fake_ch341 = FakeCh341();
    g_fake_ch341.model = model;
}

static const char *fake_ch341_pin(void) { return "fake"; }

static loraham_ch341 *fake_ch341_open(uint16_t, uint16_t, const char *)
{
    g_fake_ch341.opens++;
    return g_fake_ch341.open_ok ? reinterpret_cast<loraham_ch341 *>(&g_fake_ch341) : nullptr;
}

static void fake_ch341_close(loraham_ch341 *)
{
    g_fake_ch341.closes++;
    if (g_fake_ch341.close_log) {
        int fd = open(g_fake_ch341.close_log, O_WRONLY | O_APPEND | O_CREAT, 0600);
        if (fd >= 0) {
            ssize_t n = write(fd, "close\n", 6);
            (void)n;
            close(fd);
        }
    }
}

static int fake_ch341_transceive(loraham_ch341 *, const uint8_t *out, uint8_t *in, uint32_t len)
{
    g_fake_ch341.transceives++;
    if (g_fake_ch341.fail_transceive)
        return -1;
    g_fake_ch341.model->spiTransfer(const_cast<uint8_t *>(out), len, in);
    return 0;
}

static int fake_ch341_pin_mode(loraham_ch341 *, uint32_t pin, int output)
{
    if (pin < 8)
        g_fake_ch341.mode[pin] = output ? 1 : 0;
    return 0;
}

static int fake_ch341_digital_write(loraham_ch341 *, uint32_t pin, int level)
{
    if (pin < 8) {
        g_fake_ch341.level[pin] = level ? 1 : 0;
        if (pin == 2 && level && g_fake_ch341.writes_low[2] > 0 && g_fake_ch341.spi_at_reset_done < 0)
            g_fake_ch341.spi_at_reset_done = g_fake_ch341.transceives;
        if (level)
            g_fake_ch341.writes_high[pin]++;
        else
            g_fake_ch341.writes_low[pin]++;
    }
    return 0;
}

static int fake_ch341_digital_read(loraham_ch341 *, uint32_t pin)
{
    return pin < 8 ? g_fake_ch341.read_raw[pin] : 0;
}

static int fake_ch341_attach_interrupt(loraham_ch341 *, uint32_t pin, uint32_t, void (*cb)(void))
{
    g_fake_ch341.irq_pin = pin;
    g_fake_ch341.irq_cb = cb;
    return 0;
}

static int fake_ch341_detach_interrupt(loraham_ch341 *, uint32_t)
{
    g_fake_ch341.irq_cb = nullptr;
    return 0;
}

static int fake_ch341_in_error(loraham_ch341 *) { return g_fake_ch341.in_error ? 1 : 0; }

/* No monitor in the HAL-level fake: mon_open answers NULL (old behaviour). */
static loraham_ch341_mon *fake_mon_open(uint16_t, uint16_t) { return nullptr; }
static void fake_mon_close(loraham_ch341_mon *) { }
static int fake_mon_fds(loraham_ch341_mon *, loraham_ch341_pollfd *, int) { return 0; }
static unsigned fake_mon_gen(loraham_ch341_mon *) { return 0; }
static void fake_mon_dispatch(loraham_ch341_mon *) { }
static int fake_mon_zero(loraham_ch341_mon *) { return 0; }
static int fake_mon_bind(loraham_ch341_mon *, loraham_ch341 *) { return -1; }
static void fake_mon_unbind(loraham_ch341_mon *) { }
static void fake_mon_step(loraham_ch341_mon *, int, loraham_ch341_scan_item *) { }
static int fake_identity(loraham_ch341 *, loraham_ch341_scan_item *) { return -1; }
static void fake_seed_idle(void) { }

static const LorahamCh341Api g_fake_ch341_api = {
    fake_ch341_pin,
    fake_ch341_open,
    fake_ch341_close,
    fake_ch341_transceive,
    fake_ch341_pin_mode,
    fake_ch341_digital_write,
    fake_ch341_digital_read,
    fake_ch341_attach_interrupt,
    fake_ch341_detach_interrupt,
    fake_ch341_in_error,
    fake_mon_open,
    fake_mon_close,
    fake_mon_fds,
    fake_mon_gen,
    fake_mon_dispatch,
    fake_mon_zero,
    fake_mon_zero,
    fake_mon_zero,
    fake_mon_bind,
    fake_mon_unbind,
    fake_mon_zero,
    fake_mon_zero,
    fake_mon_step,
    fake_mon_unbind,
    fake_identity,
    fake_seed_idle,
};

#endif
