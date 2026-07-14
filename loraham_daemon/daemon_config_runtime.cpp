#include "daemon_config_runtime.h"

#include "client_slot.h"
#include "config_apply.h"
#include "daemon_log.h"
#include "daemon_protocol.h"
#include "daemon_radio_runtime.h"

/* --- External daemon CONF slots ----------------------------------------- */

extern ClientSlot client_conf433_slots[];
extern ClientSlot client_conf868_slots[];

/* --- CONFIG runtime context factories ----------------------------------- */

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

ConfigDispatchContext daemon_config_433_context(void)
{
    ConfigDispatchContext ctx = {
        client_conf433_slots,
        &radio_controller_433,
        "CONF433",
        config_apply_command,
        daemon_config_log("CONFIG433")
    };

    return ctx;
}

ConfigDispatchContext daemon_config_868_context(void)
{
    ConfigDispatchContext ctx = {
        client_conf868_slots,
        &radio_controller_868,
        "CONF868",
        config_apply_command,
        daemon_config_log("CONFIG868")
    };

    return ctx;
}
