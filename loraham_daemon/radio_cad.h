#ifndef LORAHAM_RADIO_CAD_H
#define LORAHAM_RADIO_CAD_H

#include "radio_controller.h"

/* --- Radio CAD probe helper --------------------------------------------- */

// RADIO_CAD_RSSI_BUSY_THRESHOLD_DBM (default) lives in radio_controller.h; the
// effective threshold is the per-band ctrl->cad_rssi_threshold_dbm atomic.

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

const char *radio_cad_probe_status_name(RadioCadProbeStatus status);

RadioCadProbeStatus radio_cad_status_from_scan_state(int state);

RadioCadProbeResult radio_cad_probe_unavailable(void);

void radio_cad_restore_rx_after_probe(RadioController *ctrl);

// Non-destructive monitoring probe: derives channel busy/free from a plain RSSI
// register read only. It never changes radio mode, never calls scanChannel, and
// never re-arms RX, so it cannot disturb the continuous-RX steady state. Use
// this for the periodic CAD=0/1 monitoring indicator. (scanChannel-based CAD
// remains for the deliberate, bounded TX-gating and GET CHANNEL paths.)
RadioCadProbeResult radio_cad_probe_passive(RadioController *ctrl);

RadioCadProbeResult radio_cad_try_probe(RadioController *ctrl);

RadioCadProbeResult radio_cad_probe(RadioController *ctrl);

#endif
