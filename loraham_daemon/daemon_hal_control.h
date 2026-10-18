#ifndef LORAHAM_DAEMON_HAL_CONTROL_H
#define LORAHAM_DAEMON_HAL_CONTROL_H

/* --- What lora_init() asks of a radio HAL, beyond RadioLib's interface ----- */
/*
 * RadioLib talks to a RadioLibHal. The daemon additionally needs to know
 * whether the radio's bus may be used at all, whether a startup GPIO or bus
 * call failed (a void RadioLib call cannot say), and to tell the HAL when the
 * radio is operational, from which point a lost radio I/O path is fatal and
 * restartable (exit 5) instead of a startup failure (exit 4).
 *
 * LockingPiHal (Pi SPI + lgpio) and Ch341Hal (the Meshstick over USB)
 * implement it; the controller keeps one pointer to it beside the HAL.
 */
class DaemonHalControl {
  public:
    virtual ~DaemonHalControl() = default;

    /* The bus may be used: the Pi's spi0.lock is held / the USB device is open. */
    virtual bool spi_lock_ready() const = 0;

    /* True until a startup GPIO or bus call failed. */
    virtual bool gpio_startup_ok() const = 0;

    /* The radio is up (begin + IRQ installed + RX armed). */
    virtual void gpio_mark_operational() = 0;

    /* Called from the main loop. A HAL that can lose its radio between calls
     * (the USB stick) exits 5 here once that happened; the Pi HAL learns of
     * failures in its calls and has nothing to poll. */
    virtual void check_runtime() { }

    /* Unplug-wait (the USB stick, daemon_usb_wait.h). After set_loss_wait(),
     * a failed call once the radio is operational latches a loss instead of
     * exiting 5: on_loss runs once (from whichever thread failed), every later
     * call is refused without touching the device, and the main loop decides
     * between "unplugged" and an I/O fault. Without it, nothing changes. */
    virtual void set_loss_wait(void (*on_loss)(void)) { (void)on_loss; }

    /* Called by the HAL's exit 5 after it closed the device and before
     * _exit(): the USB stick's monitor is closed there (device first). */
    virtual void set_before_exit(void (*before_exit)(void)) { (void)before_exit; }
    virtual bool loss_latched() const { return false; }

    /* The restartable runtime fatal (exit 5), closing the device first. Only
     * reached through a latched loss, which only the USB HAL produces. */
    [[noreturn]] virtual void runtime_fatal(const char *why) = 0;
};

#endif
