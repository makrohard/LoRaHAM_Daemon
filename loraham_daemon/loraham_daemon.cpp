/******************************************************************************
 * Copyright (C) 2026  [LoRaHAM / Alexander Walter]
 * * LICENSE: GNU General Public License v3 (GPLv3) with the following terms:
 * 1. PRIVATE/HOBBY: Free use, modification, and redistribution for non-commercial
 * purposes is permitted.
 * 2. COMMERCIAL: Commercial or business use is STRICTLY PROHIBITED unless a
 * written license is obtained from the author for a fee (Dual-Licensing).
 * [CONTACT: loraham.de Email Contact]
 * 3. CODE MAINTENANCE: Any modifications to this code must be reported to the
 * author (preferably via Pull Request on GitHub).
 * 4. REDISTRIBUTION: Binaries may only be distributed alongside the full
 * source code (Copyleft) (Copyleft).
 * * --- DISCLAIMER OF WARRANTY & LIMITATION OF LIABILITY ---
 * THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE. THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE
 * PROGRAM IS WITH THE USER.
 *****************************************************************************/
/*
 * Main runtime for the LoRaHAM radio daemon.
 *
 * Public interface, build notes, and examples live in README.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <getopt.h>
#include <stdarg.h>
#include <errno.h>

#include "hal/RPi/PiHal.h"
#include <RadioLib.h>

#include "daemon_protocol.h"
#include "daemon_version.h"
#include "daemon_timing.h"
#include "daemon_stats.h"
#include "daemon_lifecycle.h"
#include "daemon_radio_selection.h"
#include "daemon_radio_runtime.h"
#include "daemon_radio_init.h"
#include "daemon_tx.h"
#include "daemon_data_tx_runtime.h"
#include "daemon_log.h"
#include "daemon_led.h"
#include "data_tx.h"
#include "framed_data_tx.h"
#include "daemon_framed_data_runtime.h"
#include "daemon_rx.h"
#include "daemon_monitoring.h"
#include "framed_data.h"
#include "tx_result.h"
#include "radio_health.h"
#include "rf_packet.h"
#include "event_loop.h"
#include "unix_socket.h"
#include "client_set.h"
#include "client_slot.h"
#include "radio_channel.h"
#include "daemon_socket_runtime.h"
#include "radio_controller.h"
#include "config_dispatch.h"
#include "daemon_config_runtime.h"
#include "config_stream.h"

/* --- Global socket/client state ----------------------------------------- */
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


/* --- Channel IO state ---------------------------------------------------- */
RadioChannelIo channel_433;
RadioChannelIo channel_868;


static void daemon_shutdown_cleanup(EventLoopSet *event_set)
{
    daemon_debug_ctx("LIFE", "Stoppe Funkmodule");
    daemon_radio_shutdown_cleanup();

    daemon_debug_ctx("LIFE", "Schließe Event-Backend");
    event_loop_close(event_set);
    daemon_debug_ctx("LIFE", "Schließe Clients");

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

    daemon_debug_ctx("LIFE", "Entferne Socket-Dateien");
}

static int daemon_wait_for_events(EventLoopSet *event_set,
                                  EventLoopReadySet *readfds)
{
    int ret;

    event_loop_reset(event_set);

    if (daemon_radio_433_enabled())
        radio_channel_add_fds(&channel_433, event_set);

    if (daemon_radio_868_enabled())
        radio_channel_add_fds(&channel_868, event_set);

    ret = event_loop_wait(event_set, readfds, DAEMON_EVENT_LOOP_TIMEOUT_USEC);
    if (ret > 0)
        daemon_debug_ctx("SOCKET", "%d Event(s)", ret);

    return ret;
}

static void daemon_runtime_init(EventLoopSet *event_set)
{
    // Event backend.
    if (event_loop_init(event_set) != 0) {
        perror("epoll");
        printf("[Daemon] Event-Backend konnte nicht gestartet werden, beende.\n");
        daemon_shutdown_cleanup(event_set);
        exit(EXIT_FAILURE);
    }

    printf("[Daemon] Event-Backend: %s\n",
           event_loop_backend_name(event_loop_backend(event_set)));

    // Stop signals.
    daemon_lifecycle_reset_stop();
    if (daemon_lifecycle_install_signal_handlers() != 0) {
        perror("sigaction");
        printf("[Daemon] Signal-Handler konnten nicht gesetzt werden, beende.\n");
        daemon_shutdown_cleanup(event_set);
        exit(EXIT_FAILURE);
    }
}

static void daemon_enter_background(void)
{
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS); // Parent exits.

    if (setsid() < 0) exit(EXIT_FAILURE); // New session.

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    chdir("/");

    // Redirect stdio so sockets cannot reuse fd 0, 1 or 2.
    freopen("/dev/null", "r", stdin);
    freopen("/tmp/lora_daemon.log", "w", stdout);
    freopen("/tmp/lora_daemon.log", "w", stderr);

    daemon_debug_ctx("STARTUP", "Daemon-Modus aktiv");
}
static void daemon_startup_io_cleanup(void)
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

static void daemon_radio_io_init(void)
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

    daemon_debug_ctx("SOCKET", "Socket-Dateien öffnen");
    if (daemon_radio_433_enabled() &&
        radio_channel_open_sockets(&channel_433) != 0) {
        perror("socket 433");
        printf("[Daemon] Socket-Setup 433 fehlgeschlagen, beende.\n");
        daemon_startup_io_cleanup();
        exit(EXIT_FAILURE);
    }

    if (daemon_radio_868_enabled() &&
        radio_channel_open_sockets(&channel_868) != 0) {
        perror("socket 868");
        printf("[Daemon] Socket-Setup 868 fehlgeschlagen, beende.\n");
        daemon_startup_io_cleanup();
        exit(EXIT_FAILURE);
    }

    daemon_radio_controller_init();

    daemon_debug_ctx("GPIO", "LED initialisieren");
    daemon_led_init();

    daemon_debug_ctx("RADIO", "RadioLib initialisieren");
    lora_init();

    daemon_log_active_radios();
    if (!daemon_selected_radio_ready()) {
        printf("[Daemon] Kein ausgewähltes Radio bereit, beende.\n");
        daemon_startup_io_cleanup();
        exit(EXIT_FAILURE);
    }
}

/* --- Loop context --------------------------------------------------------- */
typedef struct {
    DaemonDeadlineTimer rssi_timer;
    DaemonDeadlineTimer stats_timer;
    DataTxDaemonContext<SX1278> data_tx_433_ctx;
    DataTxDaemonContext<RFM95> data_tx_868_ctx;
    ConfigDispatchContext<SX1278> config_433_ctx;
    ConfigDispatchContext<RFM95> config_868_ctx;
} DaemonLoopContext;

static void daemon_loop_context_init(DaemonLoopContext *ctx)
{
    long now = daemon_now_ms();

    // RSSI timer.
    daemon_deadline_timer_init(&ctx->rssi_timer,
                               now,
                               DAEMON_RSSI_INTERVAL_MS);

    // Periodic operator stats.
    daemon_stats_start(now);
    daemon_deadline_timer_init(&ctx->stats_timer,
                               now,
                               DAEMON_STATS_LOG_INTERVAL_MS);

    // DATA TX contexts.
    ctx->data_tx_433_ctx = daemon_data_tx_context(&radio_controller_433);
    ctx->data_tx_868_ctx = daemon_data_tx_context(&radio_controller_868);

    // CONFIG client slots.
    client_slot_init_all(client_conf433_slots, MAX_CLIENTS);
    client_slot_init_all(client_conf868_slots, MAX_CLIENTS);

    // CONFIG contexts.
    ctx->config_433_ctx = daemon_config_433_context();
    ctx->config_868_ctx = daemon_config_868_context();
}

/* --- Main runtime context ------------------------------------------------ */
typedef struct {
    EventLoopSet event_set;
    EventLoopReadySet readfds;
    uint8_t buf[buf_SIZE];
    uint8_t rx_buf_433[buf_SIZE];  // Separate buffer per band.
    uint8_t rx_buf_868[buf_SIZE];  // Separate buffer per band.
    DaemonLoopContext loop_ctx;
} DaemonMainContext;

static void daemon_main_context_init(DaemonMainContext *ctx)
{
    daemon_debug_ctx("LIFE", "Initialisiere Laufzeitkontext");
    daemon_runtime_init(&ctx->event_set);
    daemon_loop_context_init(&ctx->loop_ctx);
    daemon_debug_ctx("LIFE", "Laufzeitkontext bereit");
}
static void daemon_process_ready_sockets(ConfigDispatchContext<SX1278> *config_433_ctx,
                                         ConfigDispatchContext<RFM95> *config_868_ctx,
                                         DataTxDaemonContext<SX1278> *data_tx_433_ctx,
                                         DataTxDaemonContext<RFM95> *data_tx_868_ctx,
                                         const EventLoopReadySet *readfds,
                                         uint8_t *buf)
{
    if (daemon_radio_433_enabled()) {
        daemon_accept_channel_logged(&channel_433, readfds, "CLIENT433");
        data_tx_process_slots("433", client_data433_slots, MAX_CLIENTS,
                              readfds, send_data_chunk<SX1278>, data_tx_433_ctx,
                              daemon_data_tx_log("TX433"));
        daemon_process_framed_data_slots("TX433F",
                                         client_data433_framed_slots,
                                         client_data433_framed_states,
                                         MAX_CLIENTS,
                                         readfds,
                                         send_framed_data_packet<SX1278>,
                                         data_tx_433_ctx);
        config_dispatch_context<SX1278>(config_433_ctx, MAX_CLIENTS, readfds, buf);
        daemon_flush_channel_logged(&channel_433, readfds, "CLIENT433");
    }

    if (daemon_radio_868_enabled()) {
        daemon_accept_channel_logged(&channel_868, readfds, "CLIENT868");
        data_tx_process_slots("868", client_data868_slots, MAX_CLIENTS,
                              readfds, send_data_chunk<RFM95>, data_tx_868_ctx,
                              daemon_data_tx_log("TX868"));
        daemon_process_framed_data_slots("TX868F",
                                         client_data868_framed_slots,
                                         client_data868_framed_states,
                                         MAX_CLIENTS,
                                         readfds,
                                         send_framed_data_packet<RFM95>,
                                         data_tx_868_ctx);
        config_dispatch_context<RFM95>(config_868_ctx, MAX_CLIENTS, readfds, buf);
        daemon_flush_channel_logged(&channel_868, readfds, "CLIENT868");
    }
}


/* --- Main loop logging --------------------------------------------------- */
static void daemon_log_loop_start(void)
{
    printf("[Daemon] Starte Polling-Loop für LoRa und Sockets (radio=%s)\n",
           daemon_radio_selection_name(daemon_radio_selection));
}

/* --- Radio polling order ------------------------------------------------- */
static void daemon_process_radio_polling(DaemonDeadlineTimer *rssi_timer,
                                         DaemonDeadlineTimer *stats_timer,
                                         uint8_t (&rx_buf_433)[buf_SIZE],
                                         uint8_t (&rx_buf_868)[buf_SIZE])
{
    if (daemon_radio_433_enabled())
        daemon_process_radio_433(rx_buf_433);

    if (daemon_radio_868_enabled())
        daemon_process_radio_868(rx_buf_868);

    // Monitoring: CAD/RSSI/status stats.
    daemon_process_monitoring(rssi_timer, stats_timer);
}

/* --- Main loop iteration ------------------------------------------------- */
static void daemon_process_loop_iteration(EventLoopSet *event_set,
                                          EventLoopReadySet *readfds,
                                          DaemonLoopContext *loop_ctx,
                                          uint8_t *buf,
                                          uint8_t (&rx_buf_433)[buf_SIZE],
                                          uint8_t (&rx_buf_868)[buf_SIZE])
{
    // Wait for socket events.
    int ret = daemon_wait_for_events(event_set, readfds);
    if (ret < 0) {
        if (errno == EINTR && daemon_lifecycle_stop_requested()) {
            daemon_debug_ctx("LIFE", "Event-Wait durch Stop unterbrochen");
            return;
        }

        perror("event_loop_wait");
        return;
    }

    // Process ready socket clients.
    daemon_process_ready_sockets(&loop_ctx->config_433_ctx,
                                &loop_ctx->config_868_ctx,
                                &loop_ctx->data_tx_433_ctx,
                                &loop_ctx->data_tx_868_ctx,
                                readfds, buf);

    daemon_process_radio_polling(&loop_ctx->rssi_timer,
                                 &loop_ctx->stats_timer,
                                 rx_buf_433,
                                 rx_buf_868);
}

/* --- Polling loop -------------------------------------------------------- */
static void daemon_run_polling_loop(DaemonMainContext *ctx)
{
    daemon_log_loop_start();
    daemon_debug_ctx("LIFE", "Polling aktiv");

    while (!daemon_lifecycle_stop_requested()) {
        daemon_process_loop_iteration(&ctx->event_set,
                                      &ctx->readfds,
                                      &ctx->loop_ctx,
                                      ctx->buf,
                                      ctx->rx_buf_433,
                                      ctx->rx_buf_868);
    }
}

/* --- Daemon run ---------------------------------------------------------- */
static void daemon_run(void)
{
    DaemonMainContext main_ctx;
    daemon_main_context_init(&main_ctx);

    daemon_run_polling_loop(&main_ctx);

    daemon_log("Stop angefordert");
    daemon_debug_ctx("LIFE", "Shutdown beginnt");
    daemon_shutdown_cleanup(&main_ctx.event_set);
    daemon_debug_ctx("LIFE", "Shutdown abgeschlossen");
}


/* --- Startup helpers ----------------------------------------------------- */
static void daemon_ignore_sigpipe(void)
{
    // Ignore SIGPIPE; closed sockets are handled via write() errors.
    signal(SIGPIPE, SIG_IGN);
    daemon_debug_ctx("STARTUP", "SIGPIPE wird ignoriert");
}

static void daemon_print_usage(const char *argv0)
{
    printf("%s\n", LORAHAM_DAEMON_VERSION_TEXT);
    printf("\n");
    printf("Nutzung:\n");
    printf("  %s [Optionen]\n", argv0);
    printf("\n");
    printf("Optionen:\n");
    printf("  -d, --daemon     Im Hintergrund starten, Log: /tmp/lora_daemon.log\n");
    printf("  -v, --version    Version anzeigen und beenden\n");
    printf("      --debug      Debug-Log aktivieren\n");
    printf("      --radio MODE Radio wählen: both, 433, 868 (Standard: both)\n");
    printf("  -h, --help       Diese Hilfe anzeigen und beenden\n");
    printf("\n");
    printf("Sockets:\n");
    printf("  DATA  433: /tmp/lora433.sock\n");
    printf("  DATA  868: /tmp/lora868.sock\n");
    printf("  DATAF 433: /tmp/lora433f.sock\n");
    printf("  DATAF 868: /tmp/lora868f.sock\n");
    printf("  CONF  433: /tmp/loraconf433.sock\n");
    printf("  CONF  868: /tmp/loraconf868.sock\n");
    printf("\n");
}
static void daemon_print_version(void)
{
    printf("%s\n", LORAHAM_DAEMON_VERSION_TEXT);
}

static void daemon_print_startup_version(void)
{
    printf("[Daemon] %s\n", LORAHAM_DAEMON_VERSION_TEXT);
}

static bool daemon_parse_args(int argc, char *argv[])
{
    int opt;
    bool is_daemon = false;
    static const struct option long_options[] = {
        {"daemon",  no_argument, 0, 'd'},
        {"version", no_argument, 0, 'v'},
        {"debug",   no_argument, 0, 1000},
        {"radio",   required_argument, 0, 1001},
        {"help",    no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    // Parsen der Argumente
    while ((opt = getopt_long(argc, argv, "dvh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                is_daemon = true;
                daemon_debug_ctx("STARTUP", "Option -d erkannt");
                break;
            case 'v':
                daemon_print_version();
                exit(EXIT_SUCCESS);
            case 1000:
                daemon_log_level = DAEMON_LOG_DEBUG;
                daemon_debug_ctx("STARTUP", "Debug aktiv");
                break;
            case 1001:
                if (!daemon_parse_radio_selection(optarg)) {
                    fprintf(stderr, "Ungültiger Radio-Modus: %s\n", optarg ? optarg : "");
                    fprintf(stderr, "Erlaubt: both, 433, 868\n");
                    daemon_print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                daemon_debug_ctx("STARTUP", "Option --radio erkannt: %s",
                                 daemon_radio_selection_name(daemon_radio_selection));
                break;
            case 'h':
                daemon_print_usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                daemon_print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Unbekanntes Argument: %s\n", argv[optind]);
        daemon_print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    return is_daemon;
}

/* --- Startup sequence ---------------------------------------------------- */
static void daemon_startup_sequence(int argc, char *argv[])
{
    daemon_ignore_sigpipe();
    bool is_daemon = daemon_parse_args(argc, argv);

    daemon_debug_ctx("STARTUP", "Startmodus: %s", is_daemon ? "Daemon" : "Vordergrund");
    daemon_debug_ctx("STARTUP", "Radio-Auswahl: %s",
                     daemon_radio_selection_name(daemon_radio_selection));
    daemon_debug_ctx("STARTUP", "Argumente verarbeitet");

    // --- Userspace-Daemon Implementation ---
    if (is_daemon)
        daemon_enter_background();

    daemon_print_startup_version();

    daemon_debug_ctx("STARTUP", "Starte Radio- und Socket-Init");
    daemon_radio_io_init();
    daemon_debug_ctx("STARTUP", "Startup abgeschlossen");
}

/* --- Main entry ---------------------------------------------------------- */

int main(int argc, char *argv[]) {
    daemon_startup_sequence(argc, argv);
    daemon_run();

    return 0;
}
