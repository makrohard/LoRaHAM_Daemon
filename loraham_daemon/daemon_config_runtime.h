#ifndef LORAHAM_DAEMON_CONFIG_RUNTIME_H
#define LORAHAM_DAEMON_CONFIG_RUNTIME_H

#include "config_dispatch.h"

/* --- CONFIG runtime helpers --------------------------------------------- */

ConfigDispatchContext daemon_config_433_context(void);
ConfigDispatchContext daemon_config_868_context(void);

#endif
