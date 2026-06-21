#ifndef LORAHAM_DAEMON_CAD_MONITOR_BOOT_H
#define LORAHAM_DAEMON_CAD_MONITOR_BOOT_H

/* --- Boot-time CAD monitor selection ------------------------------------- */
/*
 * Per-band boot setting for the CAD=0/1 monitoring indicator, for legacy CONF
 * clients that expect CAD broadcasts but cannot issue SET CADMONITOR=1. Each
 * slot is "unset" until a flag sets it. Precedence at apply time is:
 * per-band ?? global ?? OFF, independent of argv order. Runtime SET CADMONITOR
 * can still override afterwards.
 */

typedef enum {
    DAEMON_CAD_MONITOR_BOOT_UNSET = 0,
    DAEMON_CAD_MONITOR_BOOT_OFF,
    DAEMON_CAD_MONITOR_BOOT_ON
} DaemonCadMonitorBoot;

typedef struct {
    DaemonCadMonitorBoot global;
    DaemonCadMonitorBoot band_433;
    DaemonCadMonitorBoot band_868;
} DaemonCadMonitorBootState;

extern DaemonCadMonitorBootState daemon_cad_monitor_boot;

/* Parse "on"|"off" (case-insensitive). Returns false on invalid. */
bool daemon_parse_cad_monitor_boot(const char *arg, DaemonCadMonitorBoot *out);

/* CLI setters: parse arg and store into the matching boot slot. */
bool daemon_set_cad_monitor_boot_global(const char *arg);
bool daemon_set_cad_monitor_boot_433(const char *arg);
bool daemon_set_cad_monitor_boot_868(const char *arg);

/* Resolve precedence: band ?? global ?? OFF. Returns true when monitoring on. */
bool daemon_cad_monitor_boot_resolve(DaemonCadMonitorBoot band,
                                     DaemonCadMonitorBoot global);
bool daemon_cad_monitor_boot_effective_433(void);
bool daemon_cad_monitor_boot_effective_868(void);

/* Reset all slots to UNSET (startup default / test helper). */
void daemon_cad_monitor_boot_reset(void);

#endif
