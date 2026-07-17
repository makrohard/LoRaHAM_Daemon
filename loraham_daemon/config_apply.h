#ifndef LORAHAM_CONFIG_APPLY_H
#define LORAHAM_CONFIG_APPLY_H

#include "radio_channel.h"
#include <string>
#include <atomic>
#include <stdio.h>
#include "radio_driver.h"

/* --- CONFIG apply result (audit P1-2) ------------------------------------ */
/*
 * The dispatcher decides from this whether the radio was touched: only
 * APPLIED re-arms RX (callback + startReceive). NOOP/REJECTED never touch
 * IRQ/RX state, so a malformed CONF line can no longer destroy a received,
 * undrained packet. HW_ERROR means a RadioLib setter or mode switch failed
 * mid-apply — the radio configuration is suspect and the dispatcher fails
 * the radio closed (RADIO_HEALTH_FAILED).
 */
typedef enum {
    CONFIG_APPLY_NOOP = 0,   /* nothing touched the radio */
    CONFIG_APPLY_REJECTED,   /* prevalidation rejected; radio untouched */
    CONFIG_APPLY_APPLIED,    /* success; radio touched, RX must re-arm */
    CONFIG_APPLY_HW_ERROR    /* setter/mode failure; radio state suspect */
} ConfigApplyStatus;

/* --- CONFIG apply callback --- */

using ConfigApplyFn = ConfigApplyStatus (*)(RadioDriver &radio,
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

ConfigApplyStatus parse_and_apply_config_generic(RadioDriver &radio,
                                                 const char *tag,
                                                 const char *cmd,
                                                 RadioMode_t &mode_flag,
                                                 std::atomic<bool> &getrssi_flag);

static inline ConfigApplyStatus config_apply_command(
    RadioDriver &radio,
    const char *tag,
    const char *cmd,
    RadioMode_t &mode,
    std::atomic<bool> &getrssi_active)
{
    return parse_and_apply_config_generic(radio, tag, cmd, mode,
                                          getrssi_active);
}

#endif
