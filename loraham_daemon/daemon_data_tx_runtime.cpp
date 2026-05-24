#include "daemon_data_tx_runtime.h"

#include "daemon_log.h"

/* --- DATA TX logging ----------------------------------------------------- */

void daemon_data_tx_trace_message(void *ctx, const char *msg)
{
    daemon_debug_ctx((const char *)ctx, "%s", msg);
}

DataTxLog daemon_data_tx_log(const char *ctx)
{
    DataTxLog log = {
        (void *)ctx,
        daemon_data_tx_trace_message
    };

    return log;
}
