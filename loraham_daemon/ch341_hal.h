#ifndef LORAHAM_CH341_HAL_H
#define LORAHAM_CH341_HAL_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sched.h>
#include <unistd.h>

#include <RadioLib.h>

#include "ch341_adapter.h"
#include "daemon_hal_control.h"
#include "loraham_runtime.h"

/*
 * RadioLib HAL for the ELECROW Meshstick: SPI and the radio's control lines
 * over the CH341 USB bridge, through the adapter table the loader resolved
 * (ch341_loader.h). Modelled on meshtasticd's Ch341Hal (USBHal.h, GPL-3.0,
 * meshtastic/firmware), which drives the same stick with the same library.
 *
 * Pins are the CH341's D0..D7: chip select, reset and RXEN are written as
 * GPIO (RadioLib drives CS itself), BUSY is read, DIO1 is an interrupt the
 * library polls on its own thread (about 30 times a second).
 *
 * The daemon contract is LockingPiHal's (daemon_hal_control.h):
 *   - "bus ready" is the device being open; the stick is its own USB bus, so
 *     there is no spi0.lock. Exclusive use comes from the library claiming
 *     USB interface 0: a second process cannot open the same stick.
 *   - A failed USB call before the radio is operational latches a startup
 *     failure (lora_init() refuses READY and the daemon exits 4); after it,
 *     it is fatal and exits 5, as a failed Pi SPI transfer does -- unless the
 *     daemon armed unplug-wait (set_loss_wait): then the first failure after
 *     READY latches a loss, calls on_loss once, and every later call is
 *     refused without touching the device; the main loop decides between
 *     "unplugged" and an I/O fault (daemon_usb_wait.h).
 *   - Every exit the daemon controls closes the device, which re-attaches
 *     the kernel's spi-ch341 driver: the destructor on a clean stop or a
 *     startup failure, fatal() before _exit(5).
 */
class Ch341Hal : public RadioLibHal, public DaemonHalControl {
  public:
    static const uint32_t MODE_INPUT = 0;
    static const uint32_t MODE_OUTPUT = 1;
    /* The library's pinedio_int_mode values. */
    static const uint32_t INT_RISING = 1;
    static const uint32_t INT_FALLING = 2;

    Ch341Hal(const LorahamCh341Api *api, uint16_t vid, uint16_t pid)
      : RadioLibHal(MODE_INPUT, MODE_OUTPUT, 0, 1, INT_RISING, INT_FALLING),
        _api(api)
    {
        if (_api)
            _dev = _api->open(vid, pid, nullptr);
        if (!_dev)
            fprintf(stderr, "[CH341] could not open the Meshstick %04x:%04x: not "
                            "plugged in, no permission, or in use by another "
                            "process\n", vid, pid);
    }

    /* A stick the daemon's selection already opened and checked
     * (daemon_ch341_select.h); the HAL owns it from here. */
    Ch341Hal(const LorahamCh341Api *api, loraham_ch341 *dev)
      : RadioLibHal(MODE_INPUT, MODE_OUTPUT, 0, 1, INT_RISING, INT_FALLING),
        _api(api), _dev(dev)
    {
        if (!_dev)
            fprintf(stderr, "[CH341] no Meshstick selected\n");
    }

    ~Ch341Hal() override { close_device(); }

    Ch341Hal(const Ch341Hal &) = delete;
    Ch341Hal &operator=(const Ch341Hal &) = delete;

    /* ---- DaemonHalControl ---------------------------------------------- */
    bool spi_lock_ready() const override { return _dev != nullptr; }
    bool gpio_startup_ok() const override { return !_startupFailed; }
    void gpio_mark_operational() override { _operational = true; }

    void check_runtime() override
    {
        if (_dev && _operational && !_lost && _api->in_error(_dev))
            failure("Meshstick USB device lost");
    }

    void set_loss_wait(void (*on_loss)(void)) override { _on_loss = on_loss; }
    void set_before_exit(void (*before_exit)(void)) override { _before_exit = before_exit; }
    bool loss_latched() const override { return _lost.load(); }
    [[noreturn]] void runtime_fatal(const char *why) override { fatal(why); }

    /* The open stick, for the monitor's bind (NULL when not open). */
    loraham_ch341 *device() const { return _dev; }

    /* ---- RadioLibHal ----------------------------------------------------- */
    void init() override { }
    void term() override { }

    void pinMode(uint32_t pin, uint32_t mode) override
    {
        if (pin == RADIOLIB_NC || !ready("pinMode"))
            return;
        if (_api->pin_mode(_dev, pin, mode == MODE_OUTPUT) < 0)
            failure("CH341 pin mode failed");
    }

    void digitalWrite(uint32_t pin, uint32_t value) override
    {
        if (pin == RADIOLIB_NC || !ready("digitalWrite"))
            return;
        if (_api->digital_write(_dev, pin, value != 0) < 0)
            failure("CH341 pin write failed");
    }

    uint32_t digitalRead(uint32_t pin) override
    {
        /* After a latched loss the interrupt line reads HIGH: RadioLib's
         * unbounded waits on it (SX126x::scanChannel() at the pinned commit,
         * SX126x.cpp:434-436, has no timeout) end at once, the next SPI call
         * fails, and the TX worker lets go of radio_mutex for the teardown. */
        if (_lost && pin != RADIOLIB_NC)
            return pin == _irq_pin.load() ? 1 : 0;
        if (pin == RADIOLIB_NC || !ready("digitalRead"))
            return 0;
        int level = _api->digital_read(_dev, pin);
        if (level < 0) {
            failure("CH341 pin read failed");
            /* Startup only (failure() does not return once operational): LOW
             * lets a bounded RadioLib wait time out, as in LockingPiHal. */
            return 0;
        }
        return level ? 1 : 0;
    }

    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void),
                         uint32_t mode) override
    {
        if (interruptNum == RADIOLIB_NC || !ready("attachInterrupt"))
            return;
        _irq_pin.store(interruptNum);
        if (_api->attach_interrupt(_dev, interruptNum, mode, interruptCb) < 0)
            failure("CH341 interrupt attach failed");
    }

    void detachInterrupt(uint32_t interruptNum) override
    {
        if (interruptNum == RADIOLIB_NC || !ready("detachInterrupt"))
            return;
        /* The library answers -1 when nothing was attached; RadioLib detaches
         * before every TX whether or not RX had attached, so that is not an
         * error here. */
        (void)_api->detach_interrupt(_dev, interruptNum);
    }

    void delay(RadioLibTime_t ms) override { delayMicroseconds(ms * 1000); }

    void delayMicroseconds(RadioLibTime_t us) override
    {
        /* usleep() rounds short waits up to the timer slack (about 50 us);
         * RadioLib waits a microsecond around commands. */
        if (us < 100) {
            const RadioLibTime_t until = micros() + us;
            while (micros() < until) { }
            return;
        }
        usleep((useconds_t)us);
    }

    void yield() override { sched_yield(); }

    RadioLibTime_t millis() override { return micros() / 1000; }

    RadioLibTime_t micros() override
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (RadioLibTime_t)ts.tv_sec * 1000000u + (RadioLibTime_t)(ts.tv_nsec / 1000);
    }

    long pulseIn(uint32_t, uint32_t, RadioLibTime_t) override { return 0; }

    void spiBegin() override { }
    void spiBeginTransaction() override { }
    void spiEndTransaction() override { }
    void spiEnd() override { }

    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override
    {
        if (!ready("spiTransfer")) {
            if (in)
                memset(in, 0, len);
            return;
        }
        if (_api->in_error(_dev) || _api->transceive(_dev, out, in, (uint32_t)len) < 0) {
            failure("CH341 SPI transfer failed");
            if (in)
                memset(in, 0, len);
        }
    }

  private:
    bool ready(const char *what)
    {
        if (_lost)
            return false;   /* a latched loss: refuse silently */
        if (_dev)
            return true;
        char why[96];
        snprintf(why, sizeof(why), "%s without an open Meshstick", what);
        failure(why);
        return false;
    }

    void failure(const char *why)
    {
        if (_operational && _on_loss) {
            bool expected = false;
            if (_lost.compare_exchange_strong(expected, true)) {
                /* Admission first: health and the loss flag are published
                 * before anything that could block (the log stream). */
                _on_loss();
                fprintf(stderr, "[CH341] USB failure after READY: %s - loss detected, "
                                "classifying\n", why);
                fflush(stderr);
            }
            return;
        }
        if (_operational)
            fatal(why);
        fprintf(stderr, "[CH341] startup error: %s\n", why);
        _startupFailed = true;
    }

    /* Exit 5 like LockingPiHal::fatal(), but close the device first: _exit()
     * runs no destructor, and the close is what gives the stick back to the
     * kernel driver. */
    [[noreturn]] void fatal(const char *why)
    {
        fprintf(stderr, "[RADIO] FATAL: %s - aborting to prevent unsynchronised "
                        "radio I/O\n", why);
        fflush(stderr);
        close_device();
        if (_before_exit)
            _before_exit();                 /* the monitor, after the device */
        _exit(LORAHAM_EXIT_RUNTIME_RADIO_IO_ERROR);
    }

    void close_device()
    {
        if (_dev) {
            _api->close(_dev);
            _dev = nullptr;
        }
    }

    const LorahamCh341Api *_api;
    loraham_ch341 *_dev = nullptr;
    bool _startupFailed = false;
    bool _operational = false;
    /* Unplug-wait: set from the main loop before READY; _lost may be set by
     * the TX worker (a failed transfer) or the main loop (check_runtime). */
    void (*_on_loss)(void) = nullptr;
    void (*_before_exit)(void) = nullptr;
    std::atomic<bool> _lost{false};
    std::atomic<uint32_t> _irq_pin{RADIOLIB_NC};   /* the line RadioLib attached */
};

#endif
