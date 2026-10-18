#ifndef LORAHAM_DAEMON_CH341_RUNTIME_H
#define LORAHAM_DAEMON_CH341_RUNTIME_H

#include <stddef.h>

#include "ch341_adapter.h"
#include "daemon_hal_control.h"
#include "event_loop.h"

/* --- The daemon side of the Meshstick's unplug-wait ------------------------- */
/*
 * Opens the library's monitor once (after the loader), arms the state machine
 * (daemon_usb_wait.h) with the real radio steps, selects and opens the stick
 * (daemon_ch341_select.h: the post-open checks bind it before radio init), arms
 * every Meshstick HAL the daemon creates, and puts the monitor's
 * pollfds into the main loop's epoll set. Without a monitor (no hotplug in
 * libusb) none of it is armed and a lost stick exits 5 as before.
 */

/* From argument parsing: the --usb-serial value (or NULL). */
void daemon_ch341_set_usb_serial(const char *serial);

/* In the final process, after the instance lock, before lora_init(): open the
 * monitor (arming the state machine) and select the stick. false = refused:
 * `why` is the reason (a BUSY reason starts "BUSY:"); the caller prints
 * "[CH341] refused: <why> (exit 4).", runs the startup cleanup and exits 4. */
bool daemon_ch341_start(char *why, size_t why_size);

/* After the library loaded: open the monitor and arm the state machine. */
void daemon_ch341_monitor_start(void);

/* Then select and open the stick (--usb-serial or NULL); false = refused. */
bool daemon_ch341_select_at_start(const char *usb_serial, char *why, size_t why_size);

/* The selected, checked stick for the HAL lora_init() creates (once). */
loraham_ch341 *daemon_ch341_take_selected(void);
bool daemon_ch341_monitor_active(void);

/* At shutdown, after the radio (its HAL) is released: disarm and close the
 * monitor. Nothing to do without the library. */
void daemon_ch341_shutdown(void);

/* For every Meshstick HAL lora_init() creates. */
void daemon_ch341_arm_hal(DaemonHalControl *ctl);

/* Inside the main loop's fd reconcile epoch. */
void daemon_ch341_reconcile_fds(EventLoopSet *set);

#endif
