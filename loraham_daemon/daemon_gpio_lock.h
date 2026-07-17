#ifndef LORAHAM_DAEMON_GPIO_LOCK_H
#define LORAHAM_DAEMON_GPIO_LOCK_H

#include <stddef.h>

/* --- Cross-process GPIO ownership (audit P1-1) ---------------------------- */
/*
 * lgpio's claim API prints and swallows errors, so RadioLib cannot report a
 * pin conflict — two band processes could silently fight over a line. These
 * locks provide deterministic cross-process exclusion independent of that:
 * one advisory flock per pin (gpio<N>.lock in the trusted runtime lock dir),
 * acquired non-blocking in ascending pin order (deadlock-free) BEFORE any
 * lgpio claim. A held pin fails the boot closed. The kernel releases the
 * locks on process exit; release() exists for orderly shutdown.
 */

/* Acquire locks for all pins (negatives skipped, duplicates collapsed).
 * Returns false on any conflict or lock-dir failure — caller must not
 * proceed to hardware init. Already-acquired locks are released on failure. */
bool daemon_gpio_locks_acquire(const int *pins, size_t count);

void daemon_gpio_locks_release(void);

/* Number of currently held pin locks (test/diagnostic). */
size_t daemon_gpio_locks_held(void);

#endif
