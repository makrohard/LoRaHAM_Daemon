#include "daemon_instance_lock.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "loraham_runtime.h"
#include "daemon_log.h"
#include "daemon_radio_selection.h"

/*
 * Lock-ordering contract (prevents deadlock between --radio both and split
 * per-band instances):
 *     instance-433 -> instance-868 -> spi0
 * The SPI lock (spi0) is only ever taken inside a single SPI transaction, never
 * during this long-lived instance setup, so it can never be held while we block
 * on an instance lock.
 *
 * Instance locks are non-blocking (LOCK_NB): a second same-band daemon fails
 * fast instead of queueing.
 */

static int g_lock_fd_433 = -1;
static int g_lock_fd_868 = -1;

static int instance_lock_claim(const char *band, int *out_fd)
{
    char name[64];
    int dirfd;
    int fd;

    /* Validate the trusted lock directory (root-owned in production, no symlink,
     * not group/world-writable), then create the lock file relative to it. The
     * directory is never silently created in production. */
    dirfd = loraham_open_runtime_dir();
    if (dirfd < 0)
        return LORAHAM_EXIT_LOCK_ERROR;   /* fail closed */

    snprintf(name, sizeof(name), "instance-%s.lock", band);

    fd = loraham_open_lock_file_at(dirfd, name);
    close(dirfd);
    if (fd < 0)
        return LORAHAM_EXIT_LOCK_ERROR;   /* fail closed */

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int err = errno;

        close(fd);

        if (err == EWOULDBLOCK) {
            printf("[LOCK] Band %s wird bereits von einer anderen Instanz "
                   "betrieben – beende.\n", band);
            return LORAHAM_EXIT_INSTANCE_BUSY;
        }

        printf("[LOCK] Fehler: flock(instance-%s) fehlgeschlagen: %s\n",
               band, strerror(err));
        return LORAHAM_EXIT_LOCK_ERROR;   /* fail closed */
    }

    *out_fd = fd;
    daemon_log("Instanz-Sperre erworben: %s/instance-%s.lock",
               loraham_runtime_dir(), band);
    return 0;
}

int daemon_instance_lock_acquire(void)
{
    int rc;

    /* Deterministic order: 433 before 868. */
    if (daemon_radio_433_enabled()) {
        rc = instance_lock_claim("433", &g_lock_fd_433);
        if (rc != 0)
            return rc;
    }

    if (daemon_radio_868_enabled()) {
        rc = instance_lock_claim("868", &g_lock_fd_868);
        if (rc != 0) {
            /* Roll back the 433 lock so --radio both never leaves split
             * ownership behind. */
            daemon_instance_lock_release();
            return rc;
        }
    }

    return 0;
}

void daemon_instance_lock_release(void)
{
    /* Release in reverse acquisition order. Closing the descriptor drops the
     * advisory lock; the explicit LOCK_UN is belt-and-suspenders. */
    if (g_lock_fd_868 >= 0) {
        flock(g_lock_fd_868, LOCK_UN);
        close(g_lock_fd_868);
        g_lock_fd_868 = -1;
    }

    if (g_lock_fd_433 >= 0) {
        flock(g_lock_fd_433, LOCK_UN);
        close(g_lock_fd_433);
        g_lock_fd_433 = -1;
    }
}
