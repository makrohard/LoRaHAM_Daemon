#include "daemon_tx_mode_boot.h"

#include <strings.h>

/* --- Boot-time TX mode selection ----------------------------------------- */

DaemonTxModeBootState daemon_tx_mode_boot = {
    DAEMON_TX_MODE_BOOT_UNSET,
    DAEMON_TX_MODE_BOOT_UNSET,
    DAEMON_TX_MODE_BOOT_UNSET
};

bool daemon_parse_tx_mode_boot(const char *arg, DaemonTxModeBoot *out)
{
    if (!arg)
        return false;

    if (strcasecmp(arg, "managed") == 0) {
        if (out)
            *out = DAEMON_TX_MODE_BOOT_MANAGED;
        return true;
    }

    if (strcasecmp(arg, "direct") == 0) {
        if (out)
            *out = DAEMON_TX_MODE_BOOT_DIRECT;
        return true;
    }

    return false;
}

bool daemon_set_tx_mode_boot_global(const char *arg)
{
    return daemon_parse_tx_mode_boot(arg, &daemon_tx_mode_boot.global);
}

bool daemon_set_tx_mode_boot_433(const char *arg)
{
    return daemon_parse_tx_mode_boot(arg, &daemon_tx_mode_boot.band_433);
}

bool daemon_set_tx_mode_boot_868(const char *arg)
{
    return daemon_parse_tx_mode_boot(arg, &daemon_tx_mode_boot.band_868);
}

DaemonTxModeBoot daemon_tx_mode_boot_resolve(DaemonTxModeBoot band,
                                             DaemonTxModeBoot global)
{
    if (band != DAEMON_TX_MODE_BOOT_UNSET)
        return band;

    if (global != DAEMON_TX_MODE_BOOT_UNSET)
        return global;

    return DAEMON_TX_MODE_BOOT_MANAGED;
}

DaemonTxModeBoot daemon_tx_mode_boot_effective_433(void)
{
    return daemon_tx_mode_boot_resolve(daemon_tx_mode_boot.band_433,
                                       daemon_tx_mode_boot.global);
}

DaemonTxModeBoot daemon_tx_mode_boot_effective_868(void)
{
    return daemon_tx_mode_boot_resolve(daemon_tx_mode_boot.band_868,
                                       daemon_tx_mode_boot.global);
}

void daemon_tx_mode_boot_reset(void)
{
    daemon_tx_mode_boot.global = DAEMON_TX_MODE_BOOT_UNSET;
    daemon_tx_mode_boot.band_433 = DAEMON_TX_MODE_BOOT_UNSET;
    daemon_tx_mode_boot.band_868 = DAEMON_TX_MODE_BOOT_UNSET;
}
