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
#include <stdint.h>
#include <getopt.h>
#include <errno.h>

#include <RadioLib.h>

#include "daemon_protocol.h"
#include "daemon_version.h"
#include "daemon_timing.h"
#include "daemon_stats.h"
#include "daemon_lifecycle.h"
#include "daemon_radio_selection.h"
#include "daemon_tx_mode_boot.h"
#include "daemon_cad_monitor_boot.h"
#include "daemon_cad_rssi_boot.h"
#include "daemon_radio_runtime.h"
#include "daemon_data_tx_runtime.h"
#include "daemon_log.h"
#include "daemon_rx.h"
#include "daemon_monitoring.h"
#include "daemon_io_runtime.h"
#include "daemon_instance_lock.h"
#include "event_loop.h"
#include "daemon_socket_dispatch.h"
#include "radio_controller.h"
#include "config_dispatch.h"
#include "daemon_config_runtime.h"

/* --- Shutdown cleanup ---------------------------------------------------- */
static void daemon_shutdown_cleanup(EventLoopSet *event_set)
{
    daemon_debug_ctx("LIFE", "Stoppe Funkmodule");
    daemon_radio_shutdown_cleanup();

    daemon_debug_ctx("LIFE", "Schließe Event-Backend");
    event_loop_close(event_set);

    daemon_debug_ctx("LIFE", "Schließe Clients");
    daemon_io_shutdown_cleanup();

    daemon_debug_ctx("LIFE", "Entferne Socket-Dateien");

    /* Release per-band ownership only after all sockets are closed/unlinked, so
     * a same-band restart cannot bind sockets that this instance then deletes. */
    daemon_debug_ctx("LIFE", "Gebe Instanz-Sperre frei");
    daemon_instance_lock_release();
}

/* --- Event wait/runtime -------------------------------------------------- */
static int daemon_wait_for_events(EventLoopSet *event_set,
                                   EventLoopReadySet *readfds)
{
    int ret = event_loop_wait(event_set, readfds,
                              DAEMON_EVENT_LOOP_TIMEOUT_USEC);

    if (ret > 0)
        daemon_debug_ctx("SOCKET", "%d Event(s)", ret);

    return ret;
}

static void daemon_runtime_init(EventLoopSet *event_set)
{
    // Initialize event backend.
    if (event_loop_init(event_set) != 0) {
        perror("epoll");
        printf("[Daemon] Event-Backend konnte nicht gestartet werden, beende.\n");
        daemon_shutdown_cleanup(event_set);
        exit(EXIT_FAILURE);
    }

    printf("[Daemon] Event-Backend: %s\n",
           event_loop_backend_name(event_loop_backend(event_set)));

    // Install stop signal handlers.
    daemon_lifecycle_reset_stop();
    if (daemon_lifecycle_install_signal_handlers() != 0) {
        perror("sigaction");
        printf("[Daemon] Signal-Handler konnten nicht gesetzt werden, beende.\n");
        daemon_shutdown_cleanup(event_set);
        exit(EXIT_FAILURE);
    }
}

/* --- Loop context --------------------------------------------------------- */
typedef struct {
    DaemonDeadlineTimer cad_timer;
    DaemonDeadlineTimer rssi_timer;
    DaemonDeadlineTimer stats_timer;
    DataTxDaemonContext<SX1278> data_tx_433_ctx;
    DataTxDaemonContext<RFM95> data_tx_868_ctx;
    ConfigDispatchContext<SX1278> config_433_ctx;
    ConfigDispatchContext<RFM95> config_868_ctx;
} DaemonLoopContext;

static void daemon_loop_context_init(DaemonLoopContext *ctx)
{
    DaemonTimeMs now = daemon_now_ms();    // CAD monitoring timer.
    daemon_deadline_timer_init(&ctx->cad_timer,
                                now,
                                DAEMON_CAD_POLL_INTERVAL_MS);



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
    daemon_io_sync_event_fds(&ctx->event_set);
    daemon_debug_ctx("LIFE", "Laufzeitkontext bereit");
}

/* --- Main loop logging --------------------------------------------------- */
static void daemon_log_loop_start(void)
{
    printf("[Daemon] Starte Polling-Loop für LoRa und Sockets (radio=%s)\n",
           daemon_radio_selection_name(daemon_radio_selection));
}

/* --- Radio polling order ------------------------------------------------- */
static void daemon_process_radio_polling(DaemonDeadlineTimer *cad_timer,
                                          DaemonDeadlineTimer *rssi_timer,
                                          DaemonDeadlineTimer *stats_timer,
                                         uint8_t (&rx_buf_433)[buf_SIZE],
                                         uint8_t (&rx_buf_868)[buf_SIZE])
{
    if (daemon_radio_433_enabled())
        daemon_process_radio_433(rx_buf_433);

    if (daemon_radio_868_enabled())
        daemon_process_radio_868(rx_buf_868);

    // Monitoring: CAD/RSSI/status stats.
    daemon_process_monitoring(cad_timer, rssi_timer, stats_timer);
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

        if (event_loop_registration_failed(event_set))
            daemon_lifecycle_request_stop(0);

        return;
    }

    // Process ready socket clients.
    daemon_process_ready_sockets(&loop_ctx->config_433_ctx,
                                 &loop_ctx->config_868_ctx,
                                 &loop_ctx->data_tx_433_ctx,
                                 &loop_ctx->data_tx_868_ctx,
                                 readfds, buf);

    daemon_process_radio_polling(&loop_ctx->cad_timer,
                                 &loop_ctx->rssi_timer,
                                 &loop_ctx->stats_timer,
                                 rx_buf_433,
                                 rx_buf_868);

    daemon_io_sync_event_fds(event_set);
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
    printf("      --tx-mode MODE      TX-Modus beide Bänder: direct, managed (Standard: managed)\n");
    printf("      --tx-mode-433 MODE  TX-Modus nur 433 (überschreibt --tx-mode)\n");
    printf("      --tx-mode-868 MODE  TX-Modus nur 868 (überschreibt --tx-mode)\n");
    printf("      --cad-monitor VAL      CAD=0/1-Monitor beide Bänder: on, off (Standard: off)\n");
    printf("      --cad-monitor-433 VAL  CAD-Monitor nur 433 (überschreibt --cad-monitor)\n");
    printf("      --cad-monitor-868 VAL  CAD-Monitor nur 868 (überschreibt --cad-monitor)\n");
    printf("      --cad-rssi DBM         CAD-Busy-Schwelle beide Bänder, Ganzzahl dBm -130..0 (Standard: -90)\n");
    printf("      --cad-rssi-433 DBM     CAD-Busy-Schwelle nur 433 (überschreibt --cad-rssi)\n");
    printf("      --cad-rssi-868 DBM     CAD-Busy-Schwelle nur 868 (überschreibt --cad-rssi)\n");
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
        {"daemon",      no_argument, 0, 'd'},
        {"version",     no_argument, 0, 'v'},
        {"debug",       no_argument, 0, 1000},
        {"radio",       required_argument, 0, 1001},
        {"tx-mode",     required_argument, 0, 1002},
        {"tx-mode-433", required_argument, 0, 1003},
        {"tx-mode-868", required_argument, 0, 1004},
        {"cad-monitor",     required_argument, 0, 1005},
        {"cad-monitor-433", required_argument, 0, 1006},
        {"cad-monitor-868", required_argument, 0, 1007},
        {"cad-rssi",        required_argument, 0, 1008},
        {"cad-rssi-433",    required_argument, 0, 1009},
        {"cad-rssi-868",    required_argument, 0, 1010},
        {"help",        no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    // Parse command-line options.
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
            case 1002:
                if (!daemon_set_tx_mode_boot_global(optarg)) {
                    fprintf(stderr, "Ungültiger TX-Modus: %s\n", optarg ? optarg : "");
                    fprintf(stderr, "Erlaubt: direct, managed\n");
                    daemon_print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                daemon_debug_ctx("STARTUP", "Option --tx-mode erkannt: %s", optarg);
                break;
            case 1003:
                if (!daemon_set_tx_mode_boot_433(optarg)) {
                    fprintf(stderr, "Ungültiger TX-Modus: %s\n", optarg ? optarg : "");
                    fprintf(stderr, "Erlaubt: direct, managed\n");
                    daemon_print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                daemon_debug_ctx("STARTUP", "Option --tx-mode-433 erkannt: %s", optarg);
                break;
            case 1004:
                if (!daemon_set_tx_mode_boot_868(optarg)) {
                    fprintf(stderr, "Ungültiger TX-Modus: %s\n", optarg ? optarg : "");
                    fprintf(stderr, "Erlaubt: direct, managed\n");
                    daemon_print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                daemon_debug_ctx("STARTUP", "Option --tx-mode-868 erkannt: %s", optarg);
                break;
            case 1005:
                if (!daemon_set_cad_monitor_boot_global(optarg)) {
                    fprintf(stderr, "Ungültiger CAD-Monitor-Wert: %s\n", optarg ? optarg : "");
                    fprintf(stderr, "Erlaubt: on, off\n");
                    daemon_print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                daemon_debug_ctx("STARTUP", "Option --cad-monitor erkannt: %s", optarg);
                break;
            case 1006:
                if (!daemon_set_cad_monitor_boot_433(optarg)) {
                    fprintf(stderr, "Ungültiger CAD-Monitor-Wert: %s\n", optarg ? optarg : "");
                    fprintf(stderr, "Erlaubt: on, off\n");
                    daemon_print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                daemon_debug_ctx("STARTUP", "Option --cad-monitor-433 erkannt: %s", optarg);
                break;
            case 1007:
                if (!daemon_set_cad_monitor_boot_868(optarg)) {
                    fprintf(stderr, "Ungültiger CAD-Monitor-Wert: %s\n", optarg ? optarg : "");
                    fprintf(stderr, "Erlaubt: on, off\n");
                    daemon_print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                daemon_debug_ctx("STARTUP", "Option --cad-monitor-868 erkannt: %s", optarg);
                break;
            case 1008:
                if (!daemon_set_cad_rssi_boot_global(optarg)) {
                    fprintf(stderr, "Ungültiger CAD-RSSI-Wert: %s\n", optarg ? optarg : "");
                    fprintf(stderr, "Erlaubt: Ganzzahl dBm zwischen -130 und 0\n");
                    daemon_print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                daemon_debug_ctx("STARTUP", "Option --cad-rssi erkannt: %s", optarg);
                break;
            case 1009:
                if (!daemon_set_cad_rssi_boot_433(optarg)) {
                    fprintf(stderr, "Ungültiger CAD-RSSI-Wert: %s\n", optarg ? optarg : "");
                    fprintf(stderr, "Erlaubt: Ganzzahl dBm zwischen -130 und 0\n");
                    daemon_print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                daemon_debug_ctx("STARTUP", "Option --cad-rssi-433 erkannt: %s", optarg);
                break;
            case 1010:
                if (!daemon_set_cad_rssi_boot_868(optarg)) {
                    fprintf(stderr, "Ungültiger CAD-RSSI-Wert: %s\n", optarg ? optarg : "");
                    fprintf(stderr, "Erlaubt: Ganzzahl dBm zwischen -130 und 0\n");
                    daemon_print_usage(argv[0]);
                    exit(EXIT_FAILURE);
                }
                daemon_debug_ctx("STARTUP", "Option --cad-rssi-868 erkannt: %s", optarg);
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

/* --- Boot TX mode application -------------------------------------------- */
static RadioTxMode_t daemon_boot_tx_mode_to_radio(DaemonTxModeBoot mode)
{
    return mode == DAEMON_TX_MODE_BOOT_DIRECT ? RADIO_TX_MODE_DIRECT
                                              : RADIO_TX_MODE_MANAGED;
}

// Startup-only: override the MANAGED default set by radio_controller_init with
// the CLI-resolved per-band mode. Single-threaded; runs after daemon_io_init().
static void daemon_apply_boot_tx_modes(void)
{
    RadioTxMode_t mode_433 =
        daemon_boot_tx_mode_to_radio(daemon_tx_mode_boot_effective_433());
    RadioTxMode_t mode_868 =
        daemon_boot_tx_mode_to_radio(daemon_tx_mode_boot_effective_868());

    radio_controller_433.tx_mode = mode_433;
    radio_controller_868.tx_mode = mode_868;

    daemon_debug_ctx("STARTUP", "TX-Modus 433=%s 868=%s",
                     radio_tx_mode_name(mode_433),
                     radio_tx_mode_name(mode_868));
}

/* --- Boot CAD monitor application ---------------------------------------- */
// Startup-only: apply the CLI-resolved per-band CAD monitor opt-in (default
// off). Lets legacy CONF clients get CAD=0/1 without issuing SET CADMONITOR.
// Runtime SET CADMONITOR can still override afterwards.
static void daemon_apply_boot_cad_monitor(void)
{
    bool mon_433 = daemon_cad_monitor_boot_effective_433();
    bool mon_868 = daemon_cad_monitor_boot_effective_868();

    radio_controller_433.cad_monitor_active.store(mon_433);
    radio_controller_868.cad_monitor_active.store(mon_868);

    daemon_debug_ctx("STARTUP", "CAD-Monitor 433=%d 868=%d",
                     mon_433 ? 1 : 0, mon_868 ? 1 : 0);

    // CAD RSSI threshold override (per band ?? global; unset keeps the default).
    float rssi_433 = 0.0f;
    float rssi_868 = 0.0f;
    if (daemon_cad_rssi_boot_effective_433(&rssi_433)) {
        radio_controller_433.cad_rssi_threshold_dbm.store(rssi_433);
        daemon_debug_ctx("STARTUP", "CAD-RSSI 433=%.0f", (double)rssi_433);
    }
    if (daemon_cad_rssi_boot_effective_868(&rssi_868)) {
        radio_controller_868.cad_rssi_threshold_dbm.store(rssi_868);
        daemon_debug_ctx("STARTUP", "CAD-RSSI 868=%.0f", (double)rssi_868);
    }
}

/* --- Startup sequence ---------------------------------------------------- */
static void daemon_startup_sequence(int argc, char *argv[])
{
    daemon_lifecycle_ignore_sigpipe();
    daemon_debug_ctx("STARTUP", "SIGPIPE wird ignoriert");
    bool is_daemon = daemon_parse_args(argc, argv);

    daemon_debug_ctx("STARTUP", "Startmodus: %s", is_daemon ? "Daemon" : "Vordergrund");
    daemon_debug_ctx("STARTUP", "Radio-Auswahl: %s",
                     daemon_radio_selection_name(daemon_radio_selection));
    daemon_debug_ctx("STARTUP", "Argumente verarbeitet");

    // Enter background mode when requested.
    if (is_daemon) {
        daemon_lifecycle_enter_background();
        daemon_debug_ctx("STARTUP", "Daemon-Modus aktiv");
    }

    daemon_print_startup_version();

    daemon_debug_ctx("STARTUP", "Starte Radio- und Socket-Init");
    daemon_io_init();
    daemon_apply_boot_tx_modes();
    daemon_apply_boot_cad_monitor();
    daemon_debug_ctx("STARTUP", "Startup abgeschlossen");
}

/* --- Main entry ---------------------------------------------------------- */
int main(int argc, char *argv[])
{
    daemon_startup_sequence(argc, argv);
    daemon_run();

    return 0;
}
