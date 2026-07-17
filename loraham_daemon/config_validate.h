#ifndef LORAHAM_CONFIG_VALIDATE_H
#define LORAHAM_CONFIG_VALIDATE_H

#include "config_parser.h"
#include "hardware_profile.h"   /* DaemonChipFamily */
#include "radio_channel.h"

#include <string>

/* --- Transactional CONFIG validation --- */

struct ConfigValidationResult {
    bool valid;
    std::string key;
    std::string value;
    std::string reason;
    RadioMode_t target_mode;
};

void config_validation_result_init(ConfigValidationResult *result,
                                   RadioMode_t current_mode);

/* chip_family selects family-specific value rasters (currently FSK RXBW);
 * the default keeps legacy SX127x semantics for callers without a driver. */
bool config_validate_command(const ConfigCommand &cmd,
                             RadioMode_t current_mode,
                             ConfigValidationResult *result,
                             DaemonChipFamily chip_family =
                                 DAEMON_CHIP_FAMILY_SX127X);

#endif
