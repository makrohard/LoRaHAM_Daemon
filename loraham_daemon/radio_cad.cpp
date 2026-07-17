#include "radio_cad.h"
#include "daemon_cad_monitor.h"

/* Bodies moved verbatim from radio_cad.h and daemon_cad_monitor.h (D3). */

const char *radio_cad_probe_status_name(RadioCadProbeStatus status)
{
    if (status == RADIO_CAD_PROBE_FREE)
        return "FREE";

    if (status == RADIO_CAD_PROBE_BUSY)
        return "BUSY";

    return "UNAVAILABLE";
}

RadioCadProbeStatus radio_cad_status_from_scan_state(int state)
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

RadioCadProbeResult radio_cad_probe_unavailable(void)
{
    RadioCadProbeResult result;

    result.status = RADIO_CAD_PROBE_UNAVAILABLE;
    result.scan_state = 0;
    result.scan_ran = 0;
    result.rssi_dbm = -200.0f;

    return result;
}

void radio_cad_restore_rx_after_probe(RadioController *ctrl)
{
    if (!ctrl || !ctrl->driver || !radio_controller_ready(ctrl))
        return;

    if (ctrl->mode != RADIO_MODE_LORA)
        return;

    // A CAD/scanChannel may have left an RxDone/preamble IRQ pending. Clear it
    // and the received flag before re-arming so the probe never surfaces as a
    // (re-delivered) RX packet. Order mirrors the TX-end restore in daemon_tx.
    ctrl->driver->clearIrq(0xFFFFFFFF);
    ctrl->received.store(false);
    ctrl->driver->setPacketReceivedAction(ctrl->rx_callback);
    ctrl->driver->startReceive();
}

RadioCadProbeResult radio_cad_probe_passive(RadioController *ctrl)
{
    RadioCadProbeResult result = radio_cad_probe_unavailable();

    if (!ctrl || !ctrl->driver || !radio_controller_ready(ctrl))
        return result;

    /* Lock discipline (see radio_controller.h): the monitoring tick runs in
     * the main loop and must never block behind a TX holding radio_mutex.
     * UNAVAILABLE means "state untouched", so the CAD monitor simply skips
     * this sample. */
    if (ctrl->tx_busy.load())
        return result;

    std::unique_lock<std::recursive_mutex> radio_lock(
        ctrl->radio_mutex, std::try_to_lock);
    if (!radio_lock.owns_lock())
        return result;

    if (ctrl->tx_busy.load())
        return result;

    // Live channel RSSI: the driver's rssiProbe() reads the instant RSSI
    // (current channel energy, not the stale last-packet RSSI) without
    // re-entering RX. Non-destructive, same source as the GETRSSI live stream.
    result.rssi_dbm = ctrl->driver->rssiProbe();

    if (ctrl->mode != RADIO_MODE_LORA)
        return result; // UNAVAILABLE for non-LoRa, like the active probe.

    result.scan_ran = 0;
    result.status = (result.rssi_dbm >= ctrl->cad_rssi_threshold_dbm.load())
                        ? RADIO_CAD_PROBE_BUSY
                        : RADIO_CAD_PROBE_FREE;
    return result;
}

RadioCadProbeResult radio_cad_try_probe(RadioController *ctrl)
{
    RadioCadProbeResult result = radio_cad_probe_unavailable();

    if (!ctrl || !ctrl->driver || !radio_controller_ready(ctrl))
        return result;

    /* Wiring without DIO1 (capability flag): the blocking SX127x
     * scanChannel() could never observe CadDetected and would report
     * false-FREE. Degrade defined: answer from the passive RSSI probe
     * (scan_ran stays 0, so CADSCAN=0 marks the non-scan source). */
    if (!ctrl->cad_scan_available)
        return radio_cad_probe_passive(ctrl);

    if (ctrl->tx_busy.load())
        return result;

    std::unique_lock<std::recursive_mutex> radio_lock(
        ctrl->radio_mutex, std::try_to_lock);
    if (!radio_lock.owns_lock())
        return result;

    if (ctrl->tx_busy.load())
        return result;

    result.rssi_dbm = ctrl->driver->getRSSI();

    if (ctrl->mode != RADIO_MODE_LORA)
        return result;

    ctrl->cad_active.store(true);
    result.scan_state = ctrl->driver->scanChannel();
    ctrl->cad_active.store(false);
    radio_cad_restore_rx_after_probe(ctrl);

    result.scan_ran = 1;
    result.status = radio_cad_status_from_scan_state(result.scan_state);

    return result;
}

RadioCadProbeResult radio_cad_probe(RadioController *ctrl)
{
    RadioCadProbeResult result = radio_cad_probe_unavailable();

    if (!ctrl || !ctrl->driver || !radio_controller_ready(ctrl))
        return result;

    /* No DIO1 → no trustworthy scanChannel (see radio_cad_try_probe).
     * MANAGED TX gating then runs on the passive RSSI probe: BUSY/FREE by
     * CADRSSI threshold, so LBT stays functional on such wiring. */
    if (!ctrl->cad_scan_available)
        return radio_cad_probe_passive(ctrl);

    std::lock_guard<std::recursive_mutex> radio_lock(ctrl->radio_mutex);
    result.rssi_dbm = radio_controller_packet_rssi(ctrl);

    if (ctrl->mode != RADIO_MODE_LORA)
        return result;

    ctrl->cad_active.store(true);
    result.scan_state = ctrl->driver->scanChannel();
    ctrl->cad_active.store(false);
    radio_cad_restore_rx_after_probe(ctrl);

    result.scan_ran = 1;
    result.status = radio_cad_status_from_scan_state(result.scan_state);

    return result;
}
DaemonCadMonitorTick daemon_cad_monitor_tick(
    RadioController *ctrl)
{
    DaemonCadMonitorTick tick;

    tick.edge = 0;
    tick.rssi_dbm = -200.0f;
    tick.sampled = 0;

    if (!ctrl)
        return tick;

    RadioCadProbeResult probe = radio_cad_probe_passive(ctrl);

    tick.rssi_dbm = probe.rssi_dbm;

    if (probe.status == RADIO_CAD_PROBE_UNAVAILABLE)
        return tick;

    bool was_active = ctrl->cad_broadcast_active.load();
    int free_streak = ctrl->cad_monitor_free_streak.load();
    int now_busy = daemon_monitoring_cad_next_busy(
        was_active ? 1 : 0,
        probe.rssi_dbm,
        ctrl->cad_rssi_threshold_dbm.load(),
        &free_streak);

    ctrl->cad_monitor_free_streak.store(free_streak);
    ctrl->cad_broadcast_active.store(now_busy != 0);

    tick.edge = daemon_monitoring_cad_broadcast_edge(was_active ? 1 : 0,
                                                     now_busy);
    tick.sampled = 1;

    return tick;
}
