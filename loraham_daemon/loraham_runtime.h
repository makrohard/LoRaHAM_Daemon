#ifndef LORAHAM_RUNTIME_H
#define LORAHAM_RUNTIME_H

#include <errno.h>
#include <stdlib.h>
#include <sys/file.h>

/*
 * Shared trusted runtime directory for LoRaHAM process-shared locks: the
 * per-band instance-ownership locks and the SPI-bus lock both live here.
 *
 * Production: /run/lock/loraham, provisioned root-owned (0755) by tmpfiles.d so
 * its lifetime is independent of any single daemon instance. It must NOT be a
 * systemd RuntimeDirectory: a per-unit RuntimeDirectory is removed when that
 * unit stops, which would pull the shared lock inode out from under a still
 * running peer and silently break cross-process SPI serialization.
 *
 * Override with the LORAHAM_RUNTIME_DIR environment variable for non-root
 * dev/test runs. There is deliberately no implicit /tmp fallback: a lock that
 * cannot be established in the trusted location must fail closed, never open.
 */
#define LORAHAM_RUNTIME_DIR_DEFAULT "/run/lock/loraham"

/* Process exit codes (stable; referenced by the systemd unit). */
#define LORAHAM_EXIT_INSTANCE_BUSY 3  /* same-band instance already running */
#define LORAHAM_EXIT_LOCK_ERROR    4  /* lock infrastructure failed; fail closed */

static inline const char *loraham_runtime_dir(void)
{
    const char *d = getenv("LORAHAM_RUNTIME_DIR");

    return (d && *d) ? d : LORAHAM_RUNTIME_DIR_DEFAULT;
}

/*
 * Acquire an exclusive advisory lock, retrying ONLY on EINTR. `fn` is the
 * flock implementation (real flock() in production; a fake in tests).
 *
 * Returns 0 only when the lock is confirmed held, or -1 on a hard (non-EINTR)
 * failure with errno preserved. It never returns 0 without the lock held, so
 * callers can treat a non-zero return as "must not proceed".
 */
typedef int (*loraham_flock_fn)(int fd, int operation);

static inline int loraham_flock_acquire_ex(int fd, loraham_flock_fn fn)
{
    int rc;

    do {
        rc = fn(fd, LOCK_EX);
    } while (rc < 0 && errno == EINTR);

    return rc < 0 ? -1 : 0;
}

#endif
