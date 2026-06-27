#include "daemon_io_runtime.h"

#include <stdio.h>
#include <stdlib.h>

#include "daemon_led.h"
#include "daemon_log.h"
#include "daemon_radio_init.h"
#include "daemon_radio_runtime.h"
#include "daemon_radio_selection.h"
#include "framed_data_tx.h"
#include "unix_socket.h"

/* --- Daemon I/O runtime state ------------------------------------------- */
/* Socket fd and client-slot ownership. */
int data433_fd = -1, data868_fd = -1;
int data433_framed_fd = -1, data868_framed_fd = -1;
int conf433_fd = -1, conf868_fd = -1;

ClientSlot client_data433_slots[MAX_CLIENTS];
ClientSlot client_data868_slots[MAX_CLIENTS];
ClientSlot client_data433_framed_slots[MAX_CLIENTS];
ClientSlot client_data868_framed_slots[MAX_CLIENTS];
FramedDataTxState client_data433_framed_states[MAX_CLIENTS];
FramedDataTxState client_data868_framed_states[MAX_CLIENTS];
ClientSlot client_conf433_slots[MAX_CLIENTS];
ClientSlot client_conf868_slots[MAX_CLIENTS];


/* Per-band channel descriptors. */
RadioChannelIo channel_433;
RadioChannelIo channel_868;


/* --- Shared I/O cleanup -------------------------------------------------- */
static void daemon_io_close_433(void)
{
    client_slot_close_all(client_data433_slots, MAX_CLIENTS);
    client_slot_close_all(client_data433_framed_slots, MAX_CLIENTS);
    client_slot_close_all(client_conf433_slots, MAX_CLIENTS);
    close_unix_socket(&data433_fd, DATA433_SOCKET);
    close_unix_socket(&data433_framed_fd, DATA433_FRAMED_SOCKET);
    close_unix_socket(&conf433_fd, CONF433_SOCKET);
}

static void daemon_io_close_868(void)
{
    client_slot_close_all(client_data868_slots, MAX_CLIENTS);
    client_slot_close_all(client_data868_framed_slots, MAX_CLIENTS);
    client_slot_close_all(client_conf868_slots, MAX_CLIENTS);
    close_unix_socket(&data868_fd, DATA868_SOCKET);
    close_unix_socket(&data868_framed_fd, DATA868_FRAMED_SOCKET);
    close_unix_socket(&conf868_fd, CONF868_SOCKET);
}

/* --- Daemon I/O lifecycle ----------------------------------------------- */
void daemon_io_startup_cleanup(void)
{
    daemon_debug_ctx("LIFE", "Startup-Cleanup");
    daemon_radio_shutdown_cleanup();

    if (daemon_radio_433_enabled()) {
        client_slot_close_all(client_data433_slots, MAX_CLIENTS);
        client_slot_close_all(client_data433_framed_slots, MAX_CLIENTS);
        client_slot_close_all(client_conf433_slots, MAX_CLIENTS);
        close_unix_socket(&data433_fd, DATA433_SOCKET);
        close_unix_socket(&data433_framed_fd, DATA433_FRAMED_SOCKET);
        close_unix_socket(&conf433_fd, CONF433_SOCKET);
    }

    if (daemon_radio_868_enabled()) {
        client_slot_close_all(client_data868_slots, MAX_CLIENTS);
        client_slot_close_all(client_data868_framed_slots, MAX_CLIENTS);
        client_slot_close_all(client_conf868_slots, MAX_CLIENTS);
        close_unix_socket(&data868_fd, DATA868_SOCKET);
        close_unix_socket(&data868_framed_fd, DATA868_FRAMED_SOCKET);
        close_unix_socket(&conf868_fd, CONF868_SOCKET);
    }
}


void daemon_io_shutdown_cleanup(void)
{
    if (daemon_radio_433_enabled())
        daemon_io_close_433();

    if (daemon_radio_868_enabled())
        daemon_io_close_868();
}

void daemon_io_init(void)
{
    daemon_debug_ctx("CLIENT", "Slots initialisieren");
    if (daemon_radio_433_enabled()) {
        client_slot_init_all(client_data433_slots, MAX_CLIENTS);
        client_slot_init_all(client_data433_framed_slots, MAX_CLIENTS);
        framed_data_tx_state_init_all(client_data433_framed_states, MAX_CLIENTS);
        client_slot_init_all(client_conf433_slots, MAX_CLIENTS);
    }

    if (daemon_radio_868_enabled()) {
        client_slot_init_all(client_data868_slots, MAX_CLIENTS);
        client_slot_init_all(client_data868_framed_slots, MAX_CLIENTS);
        framed_data_tx_state_init_all(client_data868_framed_states, MAX_CLIENTS);
        client_slot_init_all(client_conf868_slots, MAX_CLIENTS);
    }

    daemon_debug_ctx("RADIO", "Kanal-IO initialisieren");
    radio_channel_io_init(&channel_433,
                          RADIO_BAND_433,
                          DATA433_SOCKET,
                          DATA433_FRAMED_SOCKET,
                          CONF433_SOCKET,
                          &data433_fd,
                          &data433_framed_fd,
                          &conf433_fd,
                          client_data433_slots,
                          client_data433_framed_slots,
                          client_conf433_slots);
    radio_channel_io_init(&channel_868,
                          RADIO_BAND_868,
                          DATA868_SOCKET,
                          DATA868_FRAMED_SOCKET,
                          CONF868_SOCKET,
                          &data868_fd,
                          &data868_framed_fd,
                          &conf868_fd,
                          client_data868_slots,
                          client_data868_framed_slots,
                          client_conf868_slots);

    /*
     * Claim per-band LED/ownership BEFORE touching any socket files.  The LED
     * GPIO claim is the per-band instance lock: if another daemon already owns
     * this band the claim fails here, and we exit *before* unlinking/binding —
     * so a duplicate same-band start can never remove the live instance's
     * sockets.  A failed claim of a selected band is fatal.
     */
    daemon_debug_ctx("GPIO", "LED/Ownership initialisieren");
    if (daemon_led_init() != 0) {
        printf("[Daemon] LED/Ownership-Setup fehlgeschlagen "
               "(Band evtl. von anderer Instanz belegt), beende.\n");
        exit(EXIT_FAILURE);
    }

    daemon_debug_ctx("SOCKET", "Socket-Dateien öffnen");
    if (daemon_radio_433_enabled() &&
        radio_channel_open_sockets(&channel_433) != 0) {
        perror("socket 433");
        printf("[Daemon] Socket-Setup 433 fehlgeschlagen, beende.\n");
        daemon_io_startup_cleanup();
        exit(EXIT_FAILURE);
    }

    if (daemon_radio_868_enabled() &&
        radio_channel_open_sockets(&channel_868) != 0) {
        perror("socket 868");
        printf("[Daemon] Socket-Setup 868 fehlgeschlagen, beende.\n");
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

    if (daemon_radio_433_enabled())
        radio_channel_reconcile_fds(&channel_433, event_set);

    if (daemon_radio_868_enabled())
        radio_channel_reconcile_fds(&channel_868, event_set);

    event_loop_reconcile_end(event_set);
}
