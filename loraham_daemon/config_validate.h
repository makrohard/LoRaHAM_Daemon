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

/* chip_family selects family-specific value rasters (currently FSK RXBW).
 * freq_min/max_mhz is the band's operational frequency policy (audit P1-4,
 * from the band descriptor) — deliberately without defaults so no caller can
 * silently validate FREQ against an unbounded range. */
bool config_validate_command(const ConfigCommand &cmd,
                             RadioMode_t current_mode,
                             ConfigValidationResult *result,
                             DaemonChipFamily chip_family,
                             float freq_min_mhz,
                             float freq_max_mhz);

#endif
