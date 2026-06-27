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
    const char *dir = loraham_runtime_dir();
    char path[256];
    int n;
    int fd;

    /* Idempotent; in production the directory is pre-created root-owned by
     * tmpfiles.d. */
    mkdir(dir, 0755);

    n = snprintf(path, sizeof(path), "%s/instance-%s.lock", dir, band);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        printf("[LOCK] Fehler: Sperrpfad zu lang für Band %s\n", band);
        return LORAHAM_EXIT_LOCK_ERROR;
    }

    /* O_NOFOLLOW refuses a symlink planted at the lock path. */
    fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0660);
    if (fd < 0) {
        printf("[LOCK] Fehler: Instanz-Sperrdatei %s nicht nutzbar: %s\n",
               path, strerror(errno));
        return LORAHAM_EXIT_LOCK_ERROR;   /* fail closed */
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int err = errno;

        close(fd);

        if (err == EWOULDBLOCK) {
            printf("[LOCK] Band %s wird bereits von einer anderen Instanz "
                   "betrieben – beende.\n", band);
            return LORAHAM_EXIT_INSTANCE_BUSY;
        }

        printf("[LOCK] Fehler: flock(%s) fehlgeschlagen: %s\n",
               path, strerror(err));
        return LORAHAM_EXIT_LOCK_ERROR;   /* fail closed */
    }

    *out_fd = fd;
    daemon_log("Instanz-Sperre erworben: %s", path);
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
