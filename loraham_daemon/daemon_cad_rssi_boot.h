#ifndef LORAHAM_DAEMON_CAD_RSSI_BOOT_H
#define LORAHAM_DAEMON_CAD_RSSI_BOOT_H

/* --- Boot-time CAD RSSI threshold selection ------------------------------ */
/*
 * Per-band boot override for the CAD monitoring RSSI busy threshold (dBm).
 * Each slot is "unset" until a flag sets it. Precedence at apply time is:
 * per-band ?? global ?? (leave controller default). Runtime SET CADRSSI can
 * still override afterwards.
 */

typedef struct {
    bool set;
    float dbm;
} DaemonCadRssiBootValue;

typedef struct {
    DaemonCadRssiBootValue global;
    DaemonCadRssiBootValue band_433;
    DaemonCadRssiBootValue band_868;
} DaemonCadRssiBootState;

extern DaemonCadRssiBootState daemon_cad_rssi_boot;

/* Parse an integer dBm in [-130, 0]. Returns false on invalid. */
bool daemon_parse_cad_rssi_boot(const char *arg, float *out);

/* CLI setters: parse arg and store into the matching boot slot. */
bool daemon_set_cad_rssi_boot_global(const char *arg);
bool daemon_set_cad_rssi_boot_433(const char *arg);
bool daemon_set_cad_rssi_boot_868(const char *arg);

/*
 * Resolve precedence: band ?? global. Returns true and writes *out when a
 * threshold was requested; false when unset (caller keeps the controller
 * default).
 */
bool daemon_cad_rssi_boot_effective_433(float *out);
bool daemon_cad_rssi_boot_effective_868(float *out);

/* Reset all slots to unset (startup default / test helper). */
void daemon_cad_rssi_boot_reset(void);

#endif
