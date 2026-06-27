#ifndef LORAHAM_DAEMON_INSTANCE_LOCK_H
#define LORAHAM_DAEMON_INSTANCE_LOCK_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-band instance-ownership locks.
 *
 * Each selected band takes an exclusive advisory flock on
 *   <runtime-dir>/instance-433.lock / instance-868.lock
 * held by an open descriptor for the ENTIRE process lifetime. The descriptor is
 * released only after all sockets have been closed and unlinked, so a same-band
 * restart cannot bind new sockets (and then have them deleted) while the old
 * instance is still finishing shutdown. The kernel drops the lock automatically
 * on process death, so there is no stale-lock cleanup.
 *
 * This is the authoritative same-band ownership barrier; it does not rely on the
 * LED GPIO claim, which is released earlier during radio shutdown.
 */

/*
 * Acquire ownership locks for the selected radio(s) BEFORE any socket/GPIO/SPI
 * or radio setup. For --radio both, both locks are acquired in deterministic
 * order (433 -> 868); if the second cannot be taken, the first is rolled back
 * and the call fails (no split ownership).
 *
 * Returns 0 on success, LORAHAM_EXIT_INSTANCE_BUSY if a selected band is already
 * owned by another process, or LORAHAM_EXIT_LOCK_ERROR if the lock
 * infrastructure could not be established (fail closed). A diagnostic is printed
 * on failure.
 */
int daemon_instance_lock_acquire(void);

/*
 * Release all held instance locks. Safe to call when none are held. Must be
 * called only AFTER all sockets have been closed/unlinked.
 */
void daemon_instance_lock_release(void);

#ifdef __cplusplus
}
#endif

#endif
