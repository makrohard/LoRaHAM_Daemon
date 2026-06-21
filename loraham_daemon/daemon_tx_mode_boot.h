#ifndef LORAHAM_DAEMON_TX_MODE_BOOT_H
#define LORAHAM_DAEMON_TX_MODE_BOOT_H

/* --- Boot-time TX mode selection ----------------------------------------- */
/*
 * Per-band boot TX mode requested on the command line. Each slot is "unset"
 * until a flag sets it. Precedence at apply time is: per-band ?? global ??
 * MANAGED, independent of argv order.
 */

typedef enum {
    DAEMON_TX_MODE_BOOT_UNSET = 0,
    DAEMON_TX_MODE_BOOT_MANAGED,
    DAEMON_TX_MODE_BOOT_DIRECT
} DaemonTxModeBoot;

typedef struct {
    DaemonTxModeBoot global;
    DaemonTxModeBoot band_433;
    DaemonTxModeBoot band_868;
} DaemonTxModeBootState;

extern DaemonTxModeBootState daemon_tx_mode_boot;

/* Parse "direct"|"managed" (case-insensitive). Returns false on invalid. */
bool daemon_parse_tx_mode_boot(const char *arg, DaemonTxModeBoot *out);

/* CLI setters: parse arg and store into the matching boot slot. */
bool daemon_set_tx_mode_boot_global(const char *arg);
bool daemon_set_tx_mode_boot_433(const char *arg);
bool daemon_set_tx_mode_boot_868(const char *arg);

/* Resolve precedence: band ?? global ?? MANAGED. Never returns UNSET. */
DaemonTxModeBoot daemon_tx_mode_boot_resolve(DaemonTxModeBoot band,
                                             DaemonTxModeBoot global);
DaemonTxModeBoot daemon_tx_mode_boot_effective_433(void);
DaemonTxModeBoot daemon_tx_mode_boot_effective_868(void);

/* Reset all slots to UNSET (startup default / test helper). */
void daemon_tx_mode_boot_reset(void);

#endif
