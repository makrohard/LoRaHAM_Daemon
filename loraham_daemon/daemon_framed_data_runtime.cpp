#include "daemon_framed_data_runtime.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "client_output_queue.h"
#include "daemon_log.h"
#include "framed_data.h"

/* --- Framed DATA runtime helpers ---------------------------------------- */

static int daemon_queue_framed_error(ClientSlot *slot, const char *msg)
{
    uint8_t header[FRAMED_DATA_HEADER_LEN];
    size_t len;

    if (!slot || !msg)
        return -1;

    len = strlen(msg);
    if (len > 255)
        len = 255;

    if (framed_data_encode_header(header, sizeof(header),
                                  FRAMED_DATA_TYPE_ERROR,
                                  (uint16_t)len) != 0)
        return -1;

    if (!client_output_queue_append(&slot->output, header, sizeof(header)))
        return -1;

    if (len > 0 &&
        !client_output_queue_append(&slot->output,
                                    (const uint8_t *)msg,
                                    len))
        return -1;

    return 0;
}

typedef struct {
    ClientSlot *slot;
    const char *tag;
} DaemonFramedErrorContext;

static void daemon_framed_tx_error(const char *msg, void *ctx)
{
    DaemonFramedErrorContext *err = (DaemonFramedErrorContext *)ctx;

    if (!err || !err->slot)
        return;

    daemon_debug_ctx(err->tag ? err->tag : "TXF",
                     "Frame-Fehler: %s", msg ? msg : "");
    if (daemon_queue_framed_error(err->slot, msg ? msg : "frame error") != 0)
        client_slot_close(err->slot);
}

void daemon_process_framed_data_slots(const char *tag,
                                      ClientSlot *slots,
                                      FramedDataTxState *states,
                                      int max_clients,
                                      const EventLoopReadySet *readfds,
                                      FramedDataTxPacketHandler handler,
                                      void *ctx)
{
    if (!slots || !states)
        return;

    for (int i = 0; i < max_clients; i++) {
        ClientSlot *slot = &slots[i];

        if (!client_slot_has_client(slot)) {
            framed_data_tx_state_init(&states[i]);
            continue;
        }

        if (!client_slot_ready(slot, readfds))
            continue;

        uint8_t buf[512];
        ssize_t n;

        do {
            n = read(slot->fd, buf, sizeof(buf));
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;

            daemon_debug_ctx(tag, "Lesefehler, Client zu");
            framed_data_tx_state_init(&states[i]);
            client_slot_close(slot);
            continue;
        }

        if (n == 0) {
            daemon_debug_ctx(tag, "EOF, Client zu");
            framed_data_tx_state_init(&states[i]);
            client_slot_close(slot);
            continue;
        }

        DaemonFramedErrorContext err = { slot, tag };

        daemon_debug_ctx(tag, "Framed DATA: %zd Byte empfangen", n);
        if (framed_data_tx_feed_state(&states[i],
                                      buf,
                                      (size_t)n,
                                      handler,
                                      ctx,
                                      daemon_framed_tx_error,
                                      &err) != 0) {
            daemon_debug_ctx(tag, "TX-Handler Fehler, Client zu");
            framed_data_tx_state_init(&states[i]);
            client_slot_close(slot);
        }
    }
}
