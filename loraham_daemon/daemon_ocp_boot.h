#ifndef LORAHAM_DAEMON_OCP_BOOT_H
#define LORAHAM_DAEMON_OCP_BOOT_H

/* --- Boot-time PA over-current limit (--ocp) ------------------------------ */
/*
 * `--ocp <mA>` sets the PA over-current limit for the whole process. Without
 * it the per-family default applies: 100 mA on SX127x (the silicon default)
 * and 140 mA on SX1262 (the daemon's default). One value at every power level,
 * POWER=20 included.
 *
 * Only values the chip can represent are accepted -- nothing is rounded:
 *   SX127x  0 (protection off), 45..120 in 5 mA steps, 130..240 in 10 mA steps
 *   SX1262  5..140 in 5 mA steps (the chip has no "off"; 0 would be 0 mA)
 * Parsing happens while the arguments are read; the family check needs the
 * resolved --hw preset and runs after it.
 */

#include <stdbool.h>
#include <stddef.h>

#include "hardware_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CLI setter: a strict non-negative integer. False = not a number. */
bool daemon_set_ocp_boot_global(const char *arg);

/* True once --ocp was given. */
bool daemon_ocp_boot_given(void);

/* The value as given (meaningful only when daemon_ocp_boot_given()). */
int daemon_ocp_boot_requested_ma(void);

int daemon_ocp_default_ma(DaemonChipFamily family);

/* True when the chip of this family can represent exactly `ma`. */
bool daemon_ocp_valid_ma(DaemonChipFamily family, int ma);

/* The accepted set, for the refusal message. */
const char *daemon_ocp_valid_text(DaemonChipFamily family);

/* --ocp if given, else the family default. */
int daemon_ocp_effective_ma(DaemonChipFamily family);

/* The one startup notice line for this configuration, without the band tag.
 * False when there is nothing to say. */
bool daemon_ocp_notice(DaemonChipFamily family, bool high_power,
                       char *buf, size_t buf_size);

/* Reset to "not given" (startup default / test helper). */
void daemon_ocp_boot_reset(void);

#ifdef __cplusplus
}
#endif

#endif
