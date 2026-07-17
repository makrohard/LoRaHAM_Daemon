#include "config_dispatch.h"

#include "daemon_rx_rearm.h"

/* Bodies moved verbatim from config_dispatch.h (D3 de-inlining). */

void config_dispatch_log_bytes(const ConfigDispatchLog *log,
                                             ssize_t n)
{
    char msg[64];

    snprintf(msg, sizeof(msg), "%zd Byte empfangen", n);
    config_dispatch_log_message(log, msg);
}

void config_dispatch_log_slot(const ConfigDispatchLog *log,
                                            int index,
                                            const char *msg)
{
    char line[80];

    snprintf(line, sizeof(line), "Slot %d: %s", index, msg);
    config_dispatch_log_message(log, line);
}

/*
 * LOCK DISCIPLINE EXCEPTION (deliberate): CONFIG apply may BLOCK on
 * radio_mutex while a TX is in flight.
 *
 * The main-loop rule (radio_controller.h) says main-loop paths must gate or
 * try-lock so the loop never stalls behind the worker's blocking transmit().
 * CONFIG apply is the conscious exception: it is client-initiated, rare, and
 * must not be silently dropped — a skipped SET/CONFIG would leave the client
 * believing a configuration it never got. Blocking here for the remainder of
 * one TX (bounded by the transmit airtime) is the correct trade-off; the
 * periodic ticks (RX poll, CAD monitor, GETRSSI) skip instead.
 */
void config_dispatch_apply_line(const char *line, void *user)
{
    ConfigLineApplyContext *ctx =
        (ConfigLineApplyContext *)user;

    config_dispatch_log_line(&ctx->log, "Zeile", line);

    if(config_status_is_get_status(line)) {
        char status[512];

        config_status_format(status, sizeof(status), ctx->ctrl);
        if(!client_output_queue_append(&ctx->slot->output,
                                       (const uint8_t *)status,
                                       strlen(status))) {
            client_slot_close(ctx->slot);
            return;
        }
        client_slot_flush_output(ctx->slot);
        return;
    }

    if(config_status_is_get_stats(line)) {
        char stats[384];

        config_status_format_stats(stats, sizeof(stats), ctx->ctrl);
        if(!client_output_queue_append(&ctx->slot->output,
                                       (const uint8_t *)stats,
                                       strlen(stats))) {
            client_slot_close(ctx->slot);
            return;
        }
        client_slot_flush_output(ctx->slot);
        return;
    }

    if(config_status_is_get_channel(line)) {
        char channel[192];

        config_status_format_channel(channel, sizeof(channel), ctx->ctrl);
        if(!client_output_queue_append(&ctx->slot->output,
                                       (const uint8_t *)channel,
                                       strlen(channel))) {
            client_slot_close(ctx->slot);
            return;
        }
        client_slot_flush_output(ctx->slot);

        return;
    }

    int txresult_enabled = 0;
    if(config_status_is_set_txresult(line, &txresult_enabled)) {
        if(ctx->ctrl)
            ctx->ctrl->tx_result_active.store(txresult_enabled != 0);
        printf("[%s] TXRESULT=%d\n", ctx->tag, txresult_enabled ? 1 : 0);
        fflush(stdout);
        return;
    }

    int txqueue_enabled = 0;
    if(config_status_is_set_txqueue(line, &txqueue_enabled)) {
        if(ctx->ctrl)
            ctx->ctrl->tx_queue_active.store(txqueue_enabled != 0);
        printf("[%s] TXQUEUE=%d\n", ctx->tag, txqueue_enabled ? 1 : 0);
        fflush(stdout);
        return;
    }

    RadioTxMode_t txmode = RADIO_TX_MODE_MANAGED;
    if(config_status_is_set_txmode(line, &txmode)) {
        if(ctx->ctrl)
            ctx->ctrl->tx_mode = txmode;
        printf("[%s] TXMODE=%s\n", ctx->tag, radio_tx_mode_name(txmode));
        fflush(stdout);
        return;
    }

    int cadrssi_dbm = 0;
    if(config_status_is_set_cadrssi(line, &cadrssi_dbm)) {
        if(ctx->ctrl)
            ctx->ctrl->cad_rssi_threshold_dbm.store((float)cadrssi_dbm);
        printf("[%s] CADRSSI=%d\n", ctx->tag, cadrssi_dbm);
        fflush(stdout);
        return;
    }

    int cadmonitor_enabled = 0;
    if(config_status_is_set_cadmonitor(line, &cadmonitor_enabled)) {
        if(ctx->ctrl) {
            ctx->ctrl->cad_monitor_active.store(cadmonitor_enabled != 0);
            ctx->ctrl->cad_monitor_free_streak.store(0);
            if(!cadmonitor_enabled)
                ctx->ctrl->cad_broadcast_active.store(false);
        }
        printf("[%s] CADMONITOR=%d\n", ctx->tag, cadmonitor_enabled ? 1 : 0);
        fflush(stdout);
        return;
    }

    uint32_t cadwait_ms = 0;
    if(config_status_is_set_cadwait(line, &cadwait_ms)) {
        if(ctx->ctrl)
            ctx->ctrl->cad_wait_timeout_ms.store(cadwait_ms);
        printf("[%s] CADWAIT=%u\n", ctx->tag, (unsigned)cadwait_ms);
        fflush(stdout);
        return;
    }

    uint32_t cadidle_ms = 0;
    if(config_status_is_set_cadidle(line, &cadidle_ms)) {
        if(ctx->ctrl)
            ctx->ctrl->cad_idle_stable_ms.store(cadidle_ms);
        printf("[%s] CADIDLE=%u\n", ctx->tag, (unsigned)cadidle_ms);
        fflush(stdout);
        return;
    }

    uint32_t cadpoll_ms = 0;
    if(config_status_is_set_cadpoll(line, &cadpoll_ms)) {
        if(ctx->ctrl)
            ctx->ctrl->cad_poll_interval_ms.store(cadpoll_ms);
        printf("[%s] CADPOLL=%u\n", ctx->tag, (unsigned)cadpoll_ms);
        fflush(stdout);
        return;
    }

    int cadtx_val = 0;
    if(config_status_is_set_cadtxaftertimeout(line, &cadtx_val)) {
        if(ctx->ctrl)
            ctx->ctrl->cad_send_after_timeout.store(cadtx_val != 0);
        printf("[%s] CADTXAFTERTIMEOUT=%d\n", ctx->tag, cadtx_val ? 1 : 0);
        fflush(stdout);
        return;
    }

    if(!ctx->ctrl || !ctx->ctrl->driver ||
       !radio_controller_ready(ctx->ctrl)) {
        config_dispatch_log_message(&ctx->log, "Radio nicht bereit");
        printf("[%s] RADIO=%s CONFIG ignored\n",
               ctx->tag,
               radio_health_name(radio_controller_health(ctx->ctrl)));
        fflush(stdout);
        return;
    }

    config_dispatch_log_message(&ctx->log, "Apply startet");

    {
        std::lock_guard<std::recursive_mutex> radio_lock(ctx->ctrl->radio_mutex);

        ctx->apply_config(*ctx->ctrl->driver, ctx->tag, line,
                          ctx->ctrl->mode, ctx->ctrl->getrssi_active);

        // beginFSK()/begin() clears the IRQ callback.
        ctx->ctrl->driver->setPacketReceivedAction(ctx->ctrl->rx_callback);
        config_dispatch_log_message(&ctx->log, "Callback neu gesetzt");
        daemon_rx_rearm_note_result(ctx->ctrl,
                                    ctx->ctrl->driver->startReceive(),
                                    "CONFIG");
    }
    config_dispatch_log_message(&ctx->log, "RX neu gestartet");
}

void config_dispatch_client(ClientSlot *slots,
                                          int index,
                                          const EventLoopReadySet *readfds,
                                          uint8_t *buf,
                                          RadioController *ctrl,
                                          const char *tag,
                                          ConfigApplyFn apply_config,
                                          ConfigDispatchLog log)
{
    ClientSlot *slot = &slots[index];

    if(!client_slot_ready(slot, readfds))
        return;

    ConfigLineApplyContext line_ctx = {
        slot,
        ctrl,
        tag,
        apply_config,
        log
    };

    config_dispatch_log_slot(&log, index, "Client bereit");

    ssize_t n;

    do {
        n = read(slot->fd, buf, buf_SIZE - 1);
    } while(n < 0 && errno == EINTR);

    if(n < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        config_dispatch_log_slot(&log, index, "Lesefehler, Client zu");
        client_slot_close(slot);
        return;
    }

    if(n == 0) {
        config_dispatch_log_slot(&log, index, "EOF, Stream flush");
        if(config_stream_flush(&slot->stream,
                               config_dispatch_apply_line,
                               &line_ctx) != 0) {
            config_dispatch_log_slot(&log, index, "Flush-Fehler");
            printf("[%s] CONFIG stream flush error\n", tag);
            fflush(stdout);
        }

        client_slot_close(slot);
        config_dispatch_log_slot(&log, index, "Client geschlossen");
        return;
    }

    config_dispatch_log_bytes(&log, n);

    if(config_stream_feed(&slot->stream, buf, (size_t)n,
                          config_dispatch_apply_line,
                          &line_ctx) != 0) {
        config_dispatch_log_slot(&log, index, "Stream zu lang, Client zu");
        printf("[%s] CONFIG stream too long, client closed\n", tag);
        fflush(stdout);
        client_slot_close(slot);
        return;
    }
}

void config_dispatch_clients(ClientSlot *slots,
                                           int max_clients,
                                           const EventLoopReadySet *readfds,
                                           uint8_t *buf,
                                           RadioController *ctrl,
                                           const char *tag,
                                           ConfigApplyFn apply_config,
                                           ConfigDispatchLog log)
{
    for(int i=0;i<max_clients;i++){
        config_dispatch_client(slots, i, readfds, buf,
                               ctrl, tag,
                               apply_config, log);
    }
}

void config_dispatch_context(ConfigDispatchContext *ctx,
                                           int max_clients,
                                           const EventLoopReadySet *readfds,
                                           uint8_t *buf)
{
    config_dispatch_clients(ctx->slots,
                            max_clients, readfds, buf,
                            ctx->ctrl, ctx->tag,
                            ctx->apply_config,
                            ctx->log);
}
