#ifndef LORAHAM_CONFIG_DISPATCH_H
#define LORAHAM_CONFIG_DISPATCH_H

#include "client_slot.h"
#include "config_apply.h"
#include "daemon_protocol.h"
#include "radio_channel.h"
#include "radio_controller.h"
#include "radio_health.h"

#include <stddef.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

/* --- CONFIG client dispatch -------------------------------------------- */

template<typename RadioT>
struct ConfigDispatchContext {
    ClientSlot *slots;
    RadioController<RadioT> *ctrl;
    const char *tag;
    const char *prefix;
    ConfigApplyFn<RadioT> apply_config;
};

template<typename RadioT>
struct ConfigLineApplyContext {
    RadioController<RadioT> *ctrl;
    const char *tag;
    const char *prefix;
    ConfigApplyFn<RadioT> apply_config;
};

template<typename RadioT>
static void config_dispatch_apply_line(const char *line, void *user)
{
    ConfigLineApplyContext<RadioT> *ctx =
        (ConfigLineApplyContext<RadioT> *)user;

    if(ctx->prefix)
        printf("%s", ctx->prefix);

    if(!ctx->ctrl || !ctx->ctrl->radio ||
       !radio_controller_ready(ctx->ctrl)) {
        printf(" RADIO=%s CONFIG ignored\n",
               radio_health_name(radio_controller_health(ctx->ctrl)));
        fflush(stdout);
        return;
    }

    ctx->apply_config(*ctx->ctrl->radio, ctx->tag, line,
                      ctx->ctrl->mode, ctx->ctrl->getrssi_active);

    // beginFSK()/begin() loescht den IRQ-Callback.
    ctx->ctrl->radio->setPacketReceivedAction(ctx->ctrl->rx_callback);
    ctx->ctrl->radio->startReceive();
}

template<typename RadioT>
static void config_dispatch_client(ClientSlot *slots,
                                   int index,
                                   const EventLoopReadySet *readfds,
                                   uint8_t *buf,
                                   RadioController<RadioT> *ctrl,
                                   const char *tag,
                                   const char *prefix,
                                   ConfigApplyFn<RadioT> apply_config)
{
    ClientSlot *slot = &slots[index];

    if(!client_slot_ready(slot, readfds))
        return;

    ConfigLineApplyContext<RadioT> line_ctx = {
        ctrl,
        tag,
        prefix,
        apply_config
    };

    ssize_t n;

    do {
        n = read(slot->fd, buf, buf_SIZE - 1);
    } while(n < 0 && errno == EINTR);

    if(n < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        client_slot_close(slot);
        return;
    }

    if(n == 0) {
        if(config_stream_flush(&slot->stream,
                               config_dispatch_apply_line<RadioT>,
                               &line_ctx) != 0) {
            printf("[%s] CONFIG stream flush error\n", tag);
            fflush(stdout);
        }

        client_slot_close(slot);
        return;
    }

    if(config_stream_feed(&slot->stream, buf, (size_t)n,
                          config_dispatch_apply_line<RadioT>,
                          &line_ctx) != 0) {
        printf("[%s] CONFIG stream too long, client closed\n", tag);
        fflush(stdout);
        client_slot_close(slot);
        return;
    }
}

template<typename RadioT>
static void config_dispatch_clients(ClientSlot *slots,
                                    int max_clients,
                                    const EventLoopReadySet *readfds,
                                    uint8_t *buf,
                                    RadioController<RadioT> *ctrl,
                                    const char *tag,
                                    const char *prefix,
                                    ConfigApplyFn<RadioT> apply_config)
{
    for(int i=0;i<max_clients;i++){
        config_dispatch_client<RadioT>(slots, i, readfds, buf,
                                       ctrl, tag, prefix,
                                       apply_config);
    }
}

template<typename RadioT>
static void config_dispatch_context(ConfigDispatchContext<RadioT> *ctx,
                                    int max_clients,
                                    const EventLoopReadySet *readfds,
                                    uint8_t *buf)
{
    config_dispatch_clients<RadioT>(ctx->slots,
                                    max_clients, readfds, buf,
                                    ctx->ctrl, ctx->tag, ctx->prefix,
                                    ctx->apply_config);
}

#endif
