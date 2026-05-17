#ifndef LORAHAM_CONFIG_APPLY_H
#define LORAHAM_CONFIG_APPLY_H

#include "radio_channel.h"

/* --- CONFIG apply callback --- */

template<typename RadioT>
using ConfigApplyFn = void (*)(RadioT& radio,
                               const char *tag,
                               const char *cmd,
                               volatile RadioMode_t& mode,
                               volatile bool& getrssi_active);

#endif
