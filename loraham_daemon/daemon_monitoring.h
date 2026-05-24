#ifndef LORAHAM_DAEMON_MONITORING_H
#define LORAHAM_DAEMON_MONITORING_H

#include "daemon_timing.h"

/* --- CAD/RSSI/stats monitoring ----------------------------------------- */

void daemon_process_monitoring(DaemonDeadlineTimer *rssi_timer,
                               DaemonDeadlineTimer *stats_timer);

#endif
