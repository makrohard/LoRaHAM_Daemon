#ifndef LORAHAM_RADIO_CAD_H
#define LORAHAM_RADIO_CAD_H

#include "radio_controller.h"

/* --- Radio CAD probe helper --------------------------------------------- */

typedef enum {
    RADIO_CAD_PROBE_UNAVAILABLE = 0,
    RADIO_CAD_PROBE_FREE = 1,
    RADIO_CAD_PROBE_BUSY = 2
} RadioCadProbeStatus;

typedef struct {
    RadioCadProbeStatus status;
    int scan_state;
    int scan_ran;
    float rssi_dbm;
} RadioCadProbeResult;

static inline const char *radio_cad_probe_status_name(RadioCadProbeStatus status)
{
    if (status == RADIO_CAD_PROBE_FREE)
        return "FREE";

    if (status == RADIO_CAD_PROBE_BUSY)
        return "BUSY";

    return "UNAVAILABLE";
}

static inline RadioCadProbeStatus radio_cad_status_from_scan_state(int state)
{
#ifdef RADIOLIB_CHANNEL_FREE
    if (state == RADIOLIB_CHANNEL_FREE)
        return RADIO_CAD_PROBE_FREE;
#endif
#ifdef RADIOLIB_CHANNEL_OCCUPIED
    if (state == RADIOLIB_CHANNEL_OCCUPIED)
        return RADIO_CAD_PROBE_BUSY;
#endif
#ifdef RADIOLIB_CHANNEL_BUSY
    if (state == RADIOLIB_CHANNEL_BUSY)
        return RADIO_CAD_PROBE_BUSY;
#endif
#ifdef RADIOLIB_LORA_DETECTED
    if (state == RADIOLIB_LORA_DETECTED)
        return RADIO_CAD_PROBE_BUSY;
#endif
#ifdef RADIOLIB_PREAMBLE_DETECTED
    if (state == RADIOLIB_PREAMBLE_DETECTED)
        return RADIO_CAD_PROBE_BUSY;
#endif
#ifdef RADIOLIB_ERR_CHANNEL_BUSY
    if (state == RADIOLIB_ERR_CHANNEL_BUSY)
        return RADIO_CAD_PROBE_BUSY;
#endif

    if (state == 0)
        return RADIO_CAD_PROBE_FREE;

    if (state > 0)
        return RADIO_CAD_PROBE_BUSY;

    return RADIO_CAD_PROBE_UNAVAILABLE;
}

static inline RadioCadProbeResult radio_cad_probe_unavailable(void)
{
    RadioCadProbeResult result;

    result.status = RADIO_CAD_PROBE_UNAVAILABLE;
    result.scan_state = 0;
    result.scan_ran = 0;
    result.rssi_dbm = -200.0f;

    return result;
}

template<typename RadioT>
static inline RadioCadProbeResult radio_cad_probe(RadioController<RadioT> *ctrl)
{
    RadioCadProbeResult result = radio_cad_probe_unavailable();

    if (!ctrl || !ctrl->radio || !radio_controller_ready(ctrl))
        return result;

    std::lock_guard<std::recursive_mutex> radio_lock(ctrl->radio_mutex);
    result.rssi_dbm = radio_controller_packet_rssi(ctrl);

    if (ctrl->mode != RADIO_MODE_LORA)
        return result;

    ctrl->cad_active.store(true);
    result.scan_state = ctrl->radio->scanChannel();
    ctrl->cad_active.store(false);

    result.scan_ran = 1;
    result.status = radio_cad_status_from_scan_state(result.scan_state);

    return result;
}

#endif
