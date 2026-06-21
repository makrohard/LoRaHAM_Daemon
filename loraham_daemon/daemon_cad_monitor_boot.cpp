#include "daemon_cad_monitor_boot.h"

#include <strings.h>

/* --- Boot-time CAD monitor selection ------------------------------------- */

DaemonCadMonitorBootState daemon_cad_monitor_boot = {
    DAEMON_CAD_MONITOR_BOOT_UNSET,
    DAEMON_CAD_MONITOR_BOOT_UNSET,
    DAEMON_CAD_MONITOR_BOOT_UNSET
};

bool daemon_parse_cad_monitor_boot(const char *arg, DaemonCadMonitorBoot *out)
{
    if (!arg)
        return false;

    if (strcasecmp(arg, "on") == 0) {
        if (out)
            *out = DAEMON_CAD_MONITOR_BOOT_ON;
        return true;
    }

    if (strcasecmp(arg, "off") == 0) {
        if (out)
            *out = DAEMON_CAD_MONITOR_BOOT_OFF;
        return true;
    }

    return false;
}

bool daemon_set_cad_monitor_boot_global(const char *arg)
{
    return daemon_parse_cad_monitor_boot(arg, &daemon_cad_monitor_boot.global);
}

bool daemon_set_cad_monitor_boot_433(const char *arg)
{
    return daemon_parse_cad_monitor_boot(arg, &daemon_cad_monitor_boot.band_433);
}

bool daemon_set_cad_monitor_boot_868(const char *arg)
{
    return daemon_parse_cad_monitor_boot(arg, &daemon_cad_monitor_boot.band_868);
}

bool daemon_cad_monitor_boot_resolve(DaemonCadMonitorBoot band,
                                     DaemonCadMonitorBoot global)
{
    if (band != DAEMON_CAD_MONITOR_BOOT_UNSET)
        return band == DAEMON_CAD_MONITOR_BOOT_ON;

    if (global != DAEMON_CAD_MONITOR_BOOT_UNSET)
        return global == DAEMON_CAD_MONITOR_BOOT_ON;

    return false;
}

bool daemon_cad_monitor_boot_effective_433(void)
{
    return daemon_cad_monitor_boot_resolve(daemon_cad_monitor_boot.band_433,
                                           daemon_cad_monitor_boot.global);
}

bool daemon_cad_monitor_boot_effective_868(void)
{
    return daemon_cad_monitor_boot_resolve(daemon_cad_monitor_boot.band_868,
                                           daemon_cad_monitor_boot.global);
}

void daemon_cad_monitor_boot_reset(void)
{
    daemon_cad_monitor_boot.global = DAEMON_CAD_MONITOR_BOOT_UNSET;
    daemon_cad_monitor_boot.band_433 = DAEMON_CAD_MONITOR_BOOT_UNSET;
    daemon_cad_monitor_boot.band_868 = DAEMON_CAD_MONITOR_BOOT_UNSET;
}
