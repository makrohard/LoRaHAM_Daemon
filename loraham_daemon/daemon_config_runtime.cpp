#include "daemon_config_runtime.h"

#include "client_slot.h"
#include "config_apply.h"
#include "daemon_band.h"
#include "daemon_io_runtime.h"
#include "daemon_log.h"
#include "daemon_protocol.h"
#include "daemon_radio_runtime.h"

/* --- CONFIG runtime context factory -------------------------------------- */

static void daemon_config_trace_message(void *ctx, const char *msg)
{
    daemon_debug_ctx((const char *)ctx, "%s", msg);
}

static void daemon_config_trace_line(void *ctx,
                                     const char *msg,
                                     const char *line)
{
    daemon_debug_ctx((const char *)ctx, "%s: %s", msg, line ? line : "");
}

static ConfigDispatchLog daemon_config_log(const char *ctx)
{
    ConfigDispatchLog log = {
        (void *)ctx,
        daemon_config_trace_message,
        daemon_config_trace_line
    };

    return log;
}

ConfigDispatchContext daemon_config_context(void)
{
    const DaemonBandDescriptor *band = daemon_band();

    ConfigDispatchContext ctx = {
        client_conf_slots,
        &radio_controller,
        band->conf_log_ctx,
        config_apply_command,
        daemon_config_log(band->config_log_ctx)
    };

    return ctx;
}
