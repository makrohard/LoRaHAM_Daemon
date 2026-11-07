#include "daemon_radio_selection.h"

#include <string.h>

/* --- Radio selection ----------------------------------------------------- */

DaemonRadioSelection daemon_radio_selection = DAEMON_RADIO_SELECTION_UNSET;

const char *daemon_radio_selection_name(DaemonRadioSelection selection)
{
    switch (selection) {
        case DAEMON_RADIO_SELECTION_UNSET:
            return "unset";
        case DAEMON_RADIO_SELECTION_433:
            return "433";
        case DAEMON_RADIO_SELECTION_868:
            return "868";
        case DAEMON_RADIO_SELECTION_915:
            return "915";
    }

    return "unknown";
}

bool daemon_radio_selection_is_set(void)
{
    return daemon_radio_selection == DAEMON_RADIO_SELECTION_433 ||
           daemon_radio_selection == DAEMON_RADIO_SELECTION_868 ||
           daemon_radio_selection == DAEMON_RADIO_SELECTION_915;
}

bool daemon_radio_433_enabled(void)
{
    return daemon_radio_selection == DAEMON_RADIO_SELECTION_433;
}

bool daemon_radio_868_enabled(void)
{
    return daemon_radio_selection == DAEMON_RADIO_SELECTION_868;
}

bool daemon_radio_915_enabled(void)
{
    return daemon_radio_selection == DAEMON_RADIO_SELECTION_915;
}

RadioBand_t daemon_radio_selection_band(void)
{
    if (daemon_radio_433_enabled())
        return RADIO_BAND_433;
    return daemon_radio_915_enabled() ? RADIO_BAND_915 : RADIO_BAND_868;
}

bool daemon_parse_radio_selection(const char *arg)
{
    if (!arg)
        return false;

    if (strcmp(arg, "433") == 0) {
        daemon_radio_selection = DAEMON_RADIO_SELECTION_433;
        return true;
    }

    if (strcmp(arg, "868") == 0) {
        daemon_radio_selection = DAEMON_RADIO_SELECTION_868;
        return true;
    }

    if (strcmp(arg, "915") == 0) {
        daemon_radio_selection = DAEMON_RADIO_SELECTION_915;
        return true;
    }

    return false;
}
