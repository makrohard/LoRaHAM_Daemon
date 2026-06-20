#ifndef LORAHAM_DAEMON_MONITORING_H
#define LORAHAM_DAEMON_MONITORING_H

#include <stdint.h>

#include "daemon_timing.h"

/* --- CAD/RSSI/stats monitoring ----------------------------------------- */

static inline int daemon_monitoring_cad_probe_due(
    DaemonDeadlineTimer *cad_timer,
    long now_ms,
    int has_conf_clients)
{
    if (!cad_timer || !has_conf_clients)
        return 0;

    return daemon_deadline_timer_due(cad_timer, now_ms);
}

static inline int daemon_monitoring_cad_broadcast_edge(
    int was_active,
    int hardware_active)
{
    if (!!was_active == !!hardware_active)
        return 0;

    return hardware_active ? 1 : -1;
}

static inline int daemon_monitoring_tx_status_next_busy(
    uint32_t observed_generation)
{
    return (observed_generation & 1u) == 0u;
}

void daemon_process_monitoring(DaemonDeadlineTimer *cad_timer,
                                DaemonDeadlineTimer *rssi_timer,
                                DaemonDeadlineTimer *stats_timer);

#endif
