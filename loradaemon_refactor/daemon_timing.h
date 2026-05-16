#ifndef LORAHAM_DAEMON_TIMING_H
#define LORAHAM_DAEMON_TIMING_H

/* --- Current select-loop timing --- */

#define DAEMON_SELECT_TIMEOUT_USEC 10000

/* --- Current RSSI stream cadence --- */

#define DAEMON_RSSI_TICK_INTERVAL 10

/* --- Counter-based tick helper --- */

int daemon_tick_due(int *counter, int interval);

#endif
