#ifndef LORAHAM_DAEMON_CONFIG_RUNTIME_H
#define LORAHAM_DAEMON_CONFIG_RUNTIME_H

#include <RadioLib.h>

#include "config_dispatch.h"

/* --- CONFIG runtime helpers --------------------------------------------- */

ConfigDispatchContext<SX1278> daemon_config_433_context(void);
ConfigDispatchContext<RFM95> daemon_config_868_context(void);

#endif
