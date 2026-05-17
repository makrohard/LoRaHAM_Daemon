#ifndef LORAHAM_CONFIG_DISPATCH_H
#define LORAHAM_CONFIG_DISPATCH_H

#include "client_set.h"
#include "config_apply.h"
#include "daemon_protocol.h"
#include "radio_channel.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* --- CONFIG client dispatch -------------------------------------------- */

template<typename RadioT>
struct ConfigDispatchContext {
    int *clients;
    RadioT *radio;
    const char *tag;
    const char *prefix;
    volatile RadioMode_t *mode;
    volatile bool *getrssi_active;
    ConfigApplyFn<RadioT> apply_config;
    void (*rx_callback)(void);
};

template<typename RadioT>
static void config_dispatch_client(int *clients,
                                   int index,
                                   const EventLoopReadySet *readfds,
                                   uint8_t *buf,
                                   RadioT& radio,
                                   const char *tag,
                                   const char *prefix,
                                   volatile RadioMode_t& mode,
                                   volatile bool& getrssi_active,
                                   ConfigApplyFn<RadioT> apply_config,
                                   void (*rx_callback)(void))
{
    if(!client_set_slot_ready(clients, index, readfds))
        return;

    ssize_t n = client_set_read_slot(clients, index, buf, buf_SIZE - 1);
    if(n <= 0)
        return;

    buf[n] = '\0';

    if(prefix)
        printf("%s", prefix);

    apply_config(radio, tag, (char*)buf, mode, getrssi_active);

    // beginFSK()/begin() loescht den IRQ-Callback.
    radio.setPacketReceivedAction(rx_callback);
    radio.startReceive();
}

template<typename RadioT>
static void config_dispatch_clients(int *clients,
                                    int max_clients,
                                    const EventLoopReadySet *readfds,
                                    uint8_t *buf,
                                    RadioT& radio,
                                    const char *tag,
                                    const char *prefix,
                                    volatile RadioMode_t& mode,
                                    volatile bool& getrssi_active,
                                    ConfigApplyFn<RadioT> apply_config,
                                    void (*rx_callback)(void))
{
    for(int i=0;i<max_clients;i++){
        config_dispatch_client<RadioT>(clients, i, readfds, buf,
                                       radio, tag, prefix,
                                       mode, getrssi_active,
                                       apply_config, rx_callback);
    }
}

template<typename RadioT>
static void config_dispatch_context(ConfigDispatchContext<RadioT> *ctx,
                                    int max_clients,
                                    const EventLoopReadySet *readfds,
                                    uint8_t *buf)
{
    config_dispatch_clients<RadioT>(ctx->clients, max_clients, readfds, buf,
                                    *ctx->radio, ctx->tag, ctx->prefix,
                                    *ctx->mode, *ctx->getrssi_active,
                                    ctx->apply_config, ctx->rx_callback);
}

#endif
