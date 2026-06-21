#include "daemon_cad_rssi_boot.h"

#include <stdlib.h>
#include <string.h>

/* --- Boot-time CAD RSSI threshold selection ------------------------------ */

DaemonCadRssiBootState daemon_cad_rssi_boot = {
    { false, 0.0f },
    { false, 0.0f },
    { false, 0.0f }
};

bool daemon_parse_cad_rssi_boot(const char *arg, float *out)
{
    char *endptr;
    long v;

    if (!arg || arg[0] == '\0')
        return false;

    v = strtol(arg, &endptr, 10);
    if (*endptr != '\0' || v < -130 || v > 0)
        return false;

    if (out)
        *out = (float)v;
    return true;
}

static bool daemon_set_cad_rssi_boot_slot(const char *arg,
                                          DaemonCadRssiBootValue *slot)
{
    float dbm = 0.0f;

    if (!daemon_parse_cad_rssi_boot(arg, &dbm))
        return false;

    slot->set = true;
    slot->dbm = dbm;
    return true;
}

bool daemon_set_cad_rssi_boot_global(const char *arg)
{
    return daemon_set_cad_rssi_boot_slot(arg, &daemon_cad_rssi_boot.global);
}

bool daemon_set_cad_rssi_boot_433(const char *arg)
{
    return daemon_set_cad_rssi_boot_slot(arg, &daemon_cad_rssi_boot.band_433);
}

bool daemon_set_cad_rssi_boot_868(const char *arg)
{
    return daemon_set_cad_rssi_boot_slot(arg, &daemon_cad_rssi_boot.band_868);
}

static bool daemon_cad_rssi_boot_resolve(const DaemonCadRssiBootValue *band,
                                         const DaemonCadRssiBootValue *global,
                                         float *out)
{
    if (band->set) {
        if (out)
            *out = band->dbm;
        return true;
    }

    if (global->set) {
        if (out)
            *out = global->dbm;
        return true;
    }

    return false;
}

bool daemon_cad_rssi_boot_effective_433(float *out)
{
    return daemon_cad_rssi_boot_resolve(&daemon_cad_rssi_boot.band_433,
                                        &daemon_cad_rssi_boot.global, out);
}

bool daemon_cad_rssi_boot_effective_868(float *out)
{
    return daemon_cad_rssi_boot_resolve(&daemon_cad_rssi_boot.band_868,
                                        &daemon_cad_rssi_boot.global, out);
}

void daemon_cad_rssi_boot_reset(void)
{
    memset(&daemon_cad_rssi_boot, 0, sizeof(daemon_cad_rssi_boot));
}
