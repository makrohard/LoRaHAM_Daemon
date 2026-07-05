#ifndef LORAHAM_DAEMON_CAD_MONITOR_H
#define LORAHAM_DAEMON_CAD_MONITOR_H

#include "daemon_monitoring.h"
#include "radio_cad.h"
#include "radio_controller.h"

/* --- CAD monitor per-band tick ------------------------------------------- */
/*
 * One sample of the opt-in CAD=0/1 CONF monitor: passive RSSI probe, then the
 * de-flicker state machine from daemon_monitoring.h. Updates the published
 * latch (cad_broadcast_active) and the free-confirmation streak; the caller
 * broadcasts, logs, and reconciles the LED based on the returned edge.
 *
 * The RX received flag is deliberately not consulted: a pending packet must
 * never suppress or delay the falling CAD=0 edge.
 */

typedef struct {
    int edge;       /* +1 broadcast CAD=1, -1 broadcast CAD=0, 0 nothing */
    float rssi_dbm; /* live RSSI sampled by the passive probe */
    int sampled;    /* 0 if the probe was unavailable (state untouched) */
} DaemonCadMonitorTick;

template<typename RadioT>
static inline DaemonCadMonitorTick daemon_cad_monitor_tick(
    RadioController<RadioT> *ctrl)
{
    DaemonCadMonitorTick tick;

    tick.edge = 0;
    tick.rssi_dbm = -200.0f;
    tick.sampled = 0;

    if (!ctrl)
        return tick;

    RadioCadProbeResult probe = radio_cad_probe_passive(ctrl);

    tick.rssi_dbm = probe.rssi_dbm;

    if (probe.status == RADIO_CAD_PROBE_UNAVAILABLE)
        return tick;

    bool was_active = ctrl->cad_broadcast_active.load();
    int free_streak = ctrl->cad_monitor_free_streak.load();
    int now_busy = daemon_monitoring_cad_next_busy(
        was_active ? 1 : 0,
        probe.rssi_dbm,
        ctrl->cad_rssi_threshold_dbm.load(),
        &free_streak);

    ctrl->cad_monitor_free_streak.store(free_streak);
    ctrl->cad_broadcast_active.store(now_busy != 0);

    tick.edge = daemon_monitoring_cad_broadcast_edge(was_active ? 1 : 0,
                                                     now_busy);
    tick.sampled = 1;

    return tick;
}

#endif
