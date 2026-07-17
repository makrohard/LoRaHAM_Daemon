#include "daemon_rx_rearm.h"

#include <stdio.h>
#include <mutex>

#include "daemon_stats.h"
#include "radio_health.h"

/* --- RX re-arm robustness (audit M3) -------------------------------------- */

void daemon_rx_rearm_note_result(RadioController *ctrl, int16_t state,
                                 const char *ctx)
{
    if (!ctrl)
        return;

    if (state == 0 /* RADIOLIB_ERR_NONE */) {
        if (ctrl->rx_rearm_pending.exchange(false)) {
            printf("[%s] RX-Rearm wiederhergestellt (%s)\n",
                   radio_controller_tag(ctrl), ctx ? ctx : "?");
            fflush(stdout);
        }
        return;
    }

    daemon_radio_stats_record_rx_rearm_failure(&ctrl->stats);

    /* Latched via exchange: exactly one incident log even if two threads
     * report failures concurrently (callers hold radio_mutex today, but the
     * latch must not depend on that). */
    if (!ctrl->rx_rearm_pending.exchange(true)) {
        printf("[%s] RX-Rearm fehlgeschlagen: %d (%s) – Retry im Radio-Tick\n",
               radio_controller_tag(ctrl), (int)state, ctx ? ctx : "?");
        fflush(stdout);
    }
}

bool daemon_rx_rearm_boot_result(RadioController *ctrl, int16_t state)
{
    if (!ctrl)
        return false;

    if (state == 0 /* RADIOLIB_ERR_NONE */)
        return true;

    /* Fail closed: a READY radio that cannot enter RX is deaf; boot has no
     * recovery story, so treat it like a failed begin(). */
    ctrl->health = RADIO_HEALTH_FAILED;
    printf("[%s] RX-Start fehlgeschlagen: %d\n",
           radio_controller_tag(ctrl), (int)state);
    fflush(stdout);
    return false;
}

void daemon_rx_rearm_retry(RadioController *ctrl)
{
    if (!ctrl || !ctrl->rx_rearm_pending.load() || ctrl->tx_busy.load())
        return;

    if (!radio_controller_ready(ctrl) || !ctrl->driver)
        return;

    /* Never re-arm over an undrained packet: startReceive() would reset the
     * FIFO. The drain path re-arms anyway and clears the latch via
     * note_result. (Unreachable today — every failure path clears received
     * first — but fail safe against future callers.) */
    if (ctrl->received.load())
        return;

    std::unique_lock<std::recursive_mutex> radio_lock(
        ctrl->radio_mutex, std::try_to_lock);
    if (!radio_lock.owns_lock())
        return;

    if (ctrl->tx_busy.load())
        return;

    ctrl->driver->setPacketReceivedAction(ctrl->rx_callback);
    daemon_rx_rearm_note_result(ctrl, ctrl->driver->startReceive(), "Retry");
}
