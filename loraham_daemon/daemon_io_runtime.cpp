#include "daemon_io_runtime.h"

#include <stdio.h>
#include <stdlib.h>

#include "daemon_band.h"
#include "daemon_instance_lock.h"
#include "daemon_led.h"
#include "daemon_lifecycle.h"
#include "daemon_log.h"
#include "daemon_radio_init.h"
#include "daemon_radio_runtime.h"
#include "framed_data_tx.h"
#include "hardware_profile.h"
#include "unix_socket.h"

/* --- Daemon I/O runtime state ------------------------------------------- */
/* Socket fd and client-slot ownership for the one band of this process. */
int data_fd = -1;
int data_framed_fd = -1;
int conf_fd = -1;

ClientSlot client_data_slots[MAX_CLIENTS];
ClientSlot client_data_framed_slots[MAX_CLIENTS];
FramedDataTxState client_framed_states[MAX_CLIENTS];
ClientSlot client_conf_slots[MAX_CLIENTS];

/* Channel descriptor of the band. */
RadioChannelIo channel;

/* --- Shared I/O cleanup -------------------------------------------------- */
static void daemon_io_close(void)
{
    const DaemonBandDescriptor *band = daemon_band();

    client_slot_close_all(client_data_slots, MAX_CLIENTS);
    client_slot_close_all(client_data_framed_slots, MAX_CLIENTS);
    client_slot_close_all(client_conf_slots, MAX_CLIENTS);
    close_unix_socket(&data_fd, band->data_socket);
    close_unix_socket(&data_framed_fd, band->framed_socket);
    close_unix_socket(&conf_fd, band->conf_socket);
}

/* --- Daemon I/O lifecycle ----------------------------------------------- */
void daemon_io_startup_cleanup(void)
{
    daemon_debug_ctx("LIFE", "Startup-Cleanup");
    daemon_radio_shutdown_cleanup();

    daemon_io_close();

    /* Release instance ownership only after sockets are gone. */
    daemon_instance_lock_release();
}


void daemon_io_shutdown_cleanup(void)
{
    daemon_io_close();
}

void daemon_io_init(void)
{
    const DaemonBandDescriptor *band = daemon_band();

    /*
     * Take the per-band instance-ownership lock FIRST -- before sockets, GPIO,
     * SPI, or radio setup. This is the authoritative same-band ownership
     * barrier and is held (via an open descriptor) until after socket cleanup,
     * so a same-band restart cannot bind sockets while this instance is still
     * shutting down.
     */
    int lock_rc = daemon_instance_lock_acquire();
    if (lock_rc != 0)
        exit(lock_rc);

    /*
     * Install stop-signal handlers immediately after taking ownership and before
     * sockets/GPIO/SPI/radio setup, so a SIGTERM/SIGINT arriving during the rest
     * of startup triggers a clean lifecycle exit (the main loop observes the stop
     * flag) instead of the default-disposition kill (exit 143).
     */
    daemon_lifecycle_reset_stop();
    if (daemon_lifecycle_install_signal_handlers() != 0) {
        perror("sigaction");
        printf("[Daemon] Signal-Handler konnten nicht gesetzt werden, beende.\n");
        daemon_instance_lock_release();
        exit(EXIT_FAILURE);
    }

    daemon_debug_ctx("CLIENT", "Slots initialisieren");
    client_slot_init_all(client_data_slots, MAX_CLIENTS);
    client_slot_init_all(client_data_framed_slots, MAX_CLIENTS);
    framed_data_tx_state_init_all(client_framed_states, MAX_CLIENTS);
    client_slot_init_all(client_conf_slots, MAX_CLIENTS);

    daemon_debug_ctx("RADIO", "Kanal-IO initialisieren");
    radio_channel_io_init(&channel,
                          band->band,
                          band->data_socket,
                          band->framed_socket,
                          band->conf_socket,
                          &data_fd,
                          &data_framed_fd,
                          &conf_fd,
                          client_data_slots,
                          client_data_framed_slots,
                          client_conf_slots);

    /*
     * Claim the band's status LED (a hardware resource). Same-band ownership
     * was already secured above by the instance FD lock; this only initialises
     * the LED line. A failed claim is fatal because the LED is a required
     * hardware resource for the band.
     */
    daemon_debug_ctx("GPIO", "LED initialisieren");
    /* Route the profile's LED line to the band; led_pin < 0 = LED disabled. */
    daemon_led_configure(daemon_hw_profile.led_pin);
    if (daemon_led_init() != 0) {
        printf("[Daemon] LED-Setup fehlgeschlagen, beende.\n");
        exit(EXIT_FAILURE);
    }

    daemon_debug_ctx("SOCKET", "Socket-Dateien öffnen");
    if (radio_channel_open_sockets(&channel) != 0) {
        perror(band->band == RADIO_BAND_433 ? "socket 433" : "socket 868");
        printf("[Daemon] Socket-Setup %s fehlgeschlagen, beende.\n", band->tag);
        daemon_io_startup_cleanup();
        exit(EXIT_FAILURE);
    }

    daemon_radio_controller_init();

    daemon_debug_ctx("RADIO", "RadioLib initialisieren");
    lora_init();

    daemon_log_active_radios();
    if (!daemon_selected_radio_ready()) {
        printf("[Daemon] Kein ausgewähltes Radio bereit, beende.\n");
        daemon_io_startup_cleanup();
        exit(EXIT_FAILURE);
    }
}


void daemon_io_sync_event_fds(EventLoopSet *event_set)
{
    if (!event_set)
        return;

    event_loop_reconcile_begin(event_set);
    if (event_loop_registration_failed(event_set))
        return;

    radio_channel_reconcile_fds(&channel, event_set);

    event_loop_reconcile_end(event_set);
}
