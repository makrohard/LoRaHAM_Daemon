#ifndef LORAHAM_CH341_LOADER_H
#define LORAHAM_CH341_LOADER_H

/* --- Meshstick library loader ---------------------------------------------- */
/*
 * The Meshstick library (lib/libloraham-ch341.so, see ch341_adapter.h) is
 * optional: the daemon links neither it nor libusb. Only `--hw
 * meshstick-1262` loads it, at startup, from the bundle -- the lib/ directory
 * next to the running binary, never the library search path. Every symbol is
 * resolved at once (RTLD_NOW and one dlsym per entry), and the library must
 * report the pine64 commit this daemon was built against. Any failure leaves
 * the table untouched and gives one reason line; the caller refuses to start.
 */

#include <stddef.h>

#include "ch341_adapter.h"

/* Load the library at `path` into `api`. False with a reason in `why` when
 * the file is missing, cannot be loaded (including a missing libusb-1.0),
 * lacks a symbol, or reports a commit other than `expected_pin`. */
bool daemon_ch341_load_from(const char *path, const char *expected_pin,
                            LorahamCh341Api *api, char *why, size_t why_size);

/* `<directory of /proc/self/exe>/lib/libloraham-ch341.so`. False when the
 * executable's path cannot be read or does not fit. */
bool daemon_ch341_library_path(char *buf, size_t buf_size);

/* The startup load for this process: the bundle path, kept in a process-wide
 * table the HAL factory reads. */
bool daemon_ch341_load(const char *expected_pin, char *why, size_t why_size);

/* The table filled by daemon_ch341_load(); NULL before a successful load. */
const LorahamCh341Api *daemon_ch341_api(void);

#endif
