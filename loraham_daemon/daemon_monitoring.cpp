#include "daemon_monitoring.h"

#include <stdio.h>
#include <mutex>

#include "client_slot.h"
#include "daemon_log.h"
#include "daemon_protocol.h"
#include "daemon_radio_runtime.h"
#include "daemon_radio_selection.h"
#include "daemon_stats.h"
#include "radio_channel.h"
#include "radio_controller.h"
#include "radio_cad.h"
#include "daemon_cad_monitor.h"
#include "radio_health.h"

/* --- External daemon channel state -------------------------------------- */

extern RadioChannelIo channel_433;
extern RadioChannelIo channel_868;

/* --- CAD/RSSI log contexts ---------------------------------------------- */
template<typename RadioT>
static const char *daemon_cad_log_ctx(RadioController<RadioT> *ctrl)
{
    return (ctrl && ctrl->band == RADIO_BAND_433) ? "CAD433" : "CAD868";
}

template<typename RadioT>
static const char *daemon_rssi_log_ctx(RadioController<RadioT> *ctrl)
{
    return (ctrl && ctrl->band == RADIO_BAND_433) ? "RSSI433" : "RSSI868";
}



/* --- TX status ----------------------------------------------------------- */
template<typename RadioT>
static void daemon_process_tx_status(RadioController<RadioT> *ctrl,
                                     RadioChannelIo *io)
{
    uint32_t observed;
    uint32_t current;

    if (!ctrl || !io)
        return;

    observed = ctrl->tx_status_broadcast_generation;
    current = ctrl->tx_status_generation.load();

    while (observed != current) {
        int busy = daemon_monitoring_tx_status_next_busy(observed);

        observed++;
        ctrl->tx_status_broadcast_generation = observed;

        if (client_slot_has_clients(io->conf_slots, MAX_CLIENTS)) {
            client_slot_broadcast_queued(
                io->conf_slots,
                MAX_CLIENTS,
                busy ? "TX=1\n" : "TX=0\n");
        }
    }
}

static void daemon_process_tx_status_all(void)
{
    if (daemon_radio_433_enabled())
        daemon_process_tx_status(&radio_controller_433, &channel_433);

    if (daemon_radio_868_enabled())
        daemon_process_tx_status(&radio_controller_868, &channel_868);
}

/* --- CAD status ---------------------------------------------------------- */
template<typename RadioT>
static void daemon_process_cad_status(RadioController<RadioT> *ctrl,
                                      RadioChannelIo *io)
{
    if (!io || !client_slot_has_clients(io->conf_slots, MAX_CLIENTS))
        return;

    if (!radio_controller_ready(ctrl) || !ctrl->radio)
        return;

    if (ctrl->mode != RADIO_MODE_LORA)
        return;

    // Non-destructive RSSI probe only: the continuous monitoring tick must never
    // touch RX (no scanChannel, no startReceive). scanChannel-based CAD stays
    // for TX gating and GET CHANNEL. A pending RX packet must never suppress
    // the falling CAD=0 edge, so the tick ignores the received flag.
    DaemonCadMonitorTick tick = daemon_cad_monitor_tick(ctrl);
    const char *ctx = daemon_cad_log_ctx(ctrl);

    if (!tick.sampled)
        return;

    if (tick.edge > 0) {
        daemon_debug_ctx(ctx, "Aktiv cad=BUSY rssi=%.1f", tick.rssi_dbm);
        client_slot_broadcast_queued(io->conf_slots, MAX_CLIENTS, "CAD=1\n");
    } else if (tick.edge < 0) {
        daemon_debug_ctx(ctx, "Inaktiv cad=FREE rssi=%.1f", tick.rssi_dbm);
        client_slot_broadcast_queued(io->conf_slots, MAX_CLIENTS, "CAD=0\n");
    }

    // The LED is derived from cad_broadcast_active (and tx_busy); reconcile it
    // immediately after the source-of-truth update.
    daemon_radio_runtime_sync_led(ctrl);
}
/* --- RSSI streaming ------------------------------------------------------- */
/*
 * GETRSSI streams live RSSI on the matching CONF socket.
 * RSSI is read directly from SX127x RegRssiValue because RadioLib getRSSI()
 * reports the last packet RSSI in LoRa mode.
 *
 * Skip reads during TX. Auto-stop when no CONF client remains connected.
 */
template<typename RadioT>
static void daemon_radio_controller_getrssi_autostop(RadioChannelIo *io,
                                                     RadioController<RadioT> *ctrl)
{
    if(!client_slot_has_clients(io->conf_slots, MAX_CLIENTS) && ctrl->getrssi_active.load()) {
        const char *ctx = daemon_rssi_log_ctx(ctrl);

        ctrl->getrssi_active.store(false);
        daemon_debug_ctx(ctx, "Auto-Stop: kein Client");
    }
}

template<typename RadioT>
static void daemon_process_rssi_stream_one(RadioController<RadioT> *ctrl,
                                           RadioChannelIo *io)
{
    const char *ctx = daemon_rssi_log_ctx(ctrl);

    if (!ctrl->getrssi_active.load())
        return;

    if (ctrl->tx_busy.load()) {
        daemon_debug_ctx(ctx, "TX aktiv, überspringe");
        return;
    }

    if (!radio_controller_ready(ctrl) || !ctrl->mod) {
        daemon_debug_ctx(ctx, "Radio nicht bereit");
        return;
    }

    std::lock_guard<std::recursive_mutex> radio_lock(ctrl->radio_mutex);
    float rssi = radio_channel_read_live_rssi(ctrl->mod.get(),
                                              ctrl->mode,
                                              ctrl->is_hf);
    char rssi_msg[32];
    snprintf(rssi_msg, sizeof(rssi_msg), "RSSI=%.2f\n", rssi);
    daemon_debug_ctx(ctx, "Sende %.2f dBm", rssi);
    client_slot_broadcast_queued(io->conf_slots, MAX_CLIENTS, rssi_msg);
}

static void daemon_process_rssi_stream(DaemonDeadlineTimer *rssi_timer)
{
    if (daemon_radio_433_enabled())
        daemon_radio_controller_getrssi_autostop(&channel_433,
                                                 &radio_controller_433);

    if (daemon_radio_868_enabled())
        daemon_radio_controller_getrssi_autostop(&channel_868,
                                                 &radio_controller_868);

    // RSSI streaming is time based.
    if (daemon_deadline_timer_due(rssi_timer, daemon_now_ms())) {
        if (daemon_radio_433_enabled())
            daemon_process_rssi_stream_one(&radio_controller_433, &channel_433);

        if (daemon_radio_868_enabled())
            daemon_process_rssi_stream_one(&radio_controller_868, &channel_868);
    }
}

/* --- Periodic operator stats -------------------------------------------- */
template<typename RadioT>
static void daemon_print_radio_stats(RadioController<RadioT> *ctrl)
{
    char fields[256];
    long uptime = daemon_stats_uptime_seconds(daemon_now_ms());

    daemon_stats_format_fields(fields,
                               sizeof(fields),
                               uptime,
                               radio_controller_health(ctrl),
                               &ctrl->stats);

    printf("[STATS %s] %s\n", radio_controller_tag(ctrl), fields);
    fflush(stdout);
}

static void daemon_process_periodic_stats(DaemonDeadlineTimer *stats_timer)
{
    if (!daemon_deadline_timer_due(stats_timer, daemon_now_ms()))
        return;

    if (daemon_radio_433_enabled())
        daemon_print_radio_stats(&radio_controller_433);

    if (daemon_radio_868_enabled())
        daemon_print_radio_stats(&radio_controller_868);
}

/* --- CAD/RSSI polling ---------------------------------------------------- */
static void daemon_process_cad_rssi(DaemonDeadlineTimer *cad_timer,
                                    DaemonDeadlineTimer *rssi_timer)
{
    // Monitoring runs only on explicit opt-in (SET CADMONITOR=1). A merely
    // connected CONF client (e.g. a config-only MeshCom client) must not trigger
    // it, so it can never disturb RX.
    bool cad_433_subscribed = daemon_radio_433_enabled() &&
        client_slot_has_clients(channel_433.conf_slots, MAX_CLIENTS) &&
        radio_controller_433.cad_monitor_active.load();
    bool cad_868_subscribed = daemon_radio_868_enabled() &&
        client_slot_has_clients(channel_868.conf_slots, MAX_CLIENTS) &&
        radio_controller_868.cad_monitor_active.load();

    if (daemon_monitoring_cad_probe_due(cad_timer,
                                        daemon_now_ms(),
                                        cad_433_subscribed || cad_868_subscribed)) {
        if (cad_433_subscribed)
            daemon_process_cad_status(&radio_controller_433, &channel_433);

        if (cad_868_subscribed)
            daemon_process_cad_status(&radio_controller_868, &channel_868);
    }

    // A band that is not (or no longer) subscribed must not keep a stale busy
    // latch: the falling edge in daemon_process_cad_status only runs while
    // subscribed, so a last CONF disconnect in the busy state would otherwise
    // leave cad_broadcast_active=true (LED latched, stale CAD=1). Clear it and
    // reconcile the LED; cad_monitor_active stays armed for a reconnect.
    if (daemon_radio_433_enabled() && !cad_433_subscribed) {
        radio_controller_433.cad_broadcast_active.store(false);
        radio_controller_433.cad_monitor_free_streak.store(0);
        daemon_radio_runtime_sync_led(&radio_controller_433);
    }
    if (daemon_radio_868_enabled() && !cad_868_subscribed) {
        radio_controller_868.cad_broadcast_active.store(false);
        radio_controller_868.cad_monitor_free_streak.store(0);
        daemon_radio_runtime_sync_led(&radio_controller_868);
    }

    daemon_process_rssi_stream(rssi_timer);
}

/* --- Per-loop LED reconciliation ----------------------------------------- */
// Safety net: drive each enabled band's LED from the derived state every loop,
// so a missed edge can never leave the pin latched.
static void daemon_sync_band_leds(void)
{
    if (daemon_radio_433_enabled())
        daemon_radio_runtime_sync_led(&radio_controller_433);

    if (daemon_radio_868_enabled())
        daemon_radio_runtime_sync_led(&radio_controller_868);
}

/* --- Monitoring tick ----------------------------------------------------- */
void daemon_process_monitoring(DaemonDeadlineTimer *cad_timer,
                                DaemonDeadlineTimer *rssi_timer,
                                DaemonDeadlineTimer *stats_timer)
{
    daemon_process_tx_status_all();
    daemon_process_cad_rssi(cad_timer, rssi_timer);
    daemon_process_periodic_stats(stats_timer);
    daemon_sync_band_leds();
}
