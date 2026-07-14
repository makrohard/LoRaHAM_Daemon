#ifndef LORAHAM_CONFIG_APPLY_H
#define LORAHAM_CONFIG_APPLY_H

#include "radio_channel.h"
#include <string>
#include <atomic>
#include <stdio.h>
#include "radio_driver.h"

/* --- CONFIG apply callback --- */

using ConfigApplyFn = void (*)(RadioDriver &radio,
                               const char *tag,
                               const char *cmd,
                               RadioMode_t &mode,
                               std::atomic<bool> &getrssi_active);

/* --- CONFIG command apply ------------------------------------------------ */
// Apply MODE first, then GETRSSI, then mode-specific radio parameters.
// SET without MODE keeps the current mode for backward compatibility.
// The chip-specific parameter setters live in the RadioDriver implementation
// (applyLoraParam/applyFskParam/switchMode); this layer stays chip-agnostic.
static inline void config_apply_print_prefix(const char *tag, bool *printed)
{
    if (!*printed) {
        printf("[%s]", tag);
        *printed = true;
    }
}

void parse_and_apply_config_generic(RadioDriver &radio, const char *tag,
                                    const char *cmd,
                                    RadioMode_t &mode_flag,
                                    std::atomic<bool> &getrssi_flag);

static inline void config_apply_command(RadioDriver &radio,
                                        const char *tag,
                                        const char *cmd,
                                        RadioMode_t &mode,
                                        std::atomic<bool> &getrssi_active)
{
    parse_and_apply_config_generic(radio, tag, cmd, mode, getrssi_active);
}

#endif
