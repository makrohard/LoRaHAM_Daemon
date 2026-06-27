#ifndef LORAHAM_LOCKING_PIHAL_H
#define LORAHAM_LOCKING_PIHAL_H

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hal/RPi/PiHal.h"
#include "loraham_runtime.h"

/*
 * PiHal that serializes every SPI transaction across processes (and across
 * bands within one process) using a process-shared advisory flock.
 *
 * Why this is the correct interception point:
 *   RadioLib's Module brackets each transfer as
 *       spiBeginTransaction(); CS-low; spiTransfer(); CS-high; spiEndTransaction();
 *   (RadioLib/src/Module.cpp). Taking the lock in spiBeginTransaction() and
 *   releasing it in spiEndTransaction() protects exactly one complete SPI
 *   transaction -- never the whole daemon lifetime. The bus is free between
 *   transactions, so complete transactions to the two radios (each on its own
 *   CS line) may interleave, which is what a shared SPI bus permits.
 *
 * Why per-instance descriptors:
 *   Each radio constructs its own LockingPiHal with its own open descriptor on
 *   the same lock file; flock() on distinct open file descriptions is mutually
 *   exclusive even within one process, giving correct serialization both between
 *   the two bands of a --radio both process and between two separate per-band
 *   daemons sharing /dev/spidev0.0.
 *
 * FAIL CLOSED (production invariant):
 *   No SPI transfer may proceed unless the process-shared lock is confirmed
 *   held. If the trusted lock directory/file cannot be opened, spi_lock_ready()
 *   returns false and the daemon refuses to start that radio. There is no /tmp
 *   fallback. A hard (non-EINTR) flock() failure, or any attempt to transfer
 *   without the lock held, triggers a controlled fatal exit rather than an
 *   unsynchronized bus access.
 *
 * Lock ordering: callers hold the per-band radio_mutex before any SPI call, and
 * this flock is the lowest-level lock taken last -- no inverse path, no deadlock.
 */
class LockingPiHal : public PiHal {
  public:
    LockingPiHal(uint8_t spiChannel, uint32_t spiSpeed = 2000000,
                 uint8_t spiDevice = 0, uint8_t gpioDevice = 0)
      : PiHal(spiChannel, spiSpeed, spiDevice, gpioDevice) {
        open_lock();
    }

    ~LockingPiHal() override {
        if (_lockFd >= 0) {
            flock(_lockFd, LOCK_UN);
            close(_lockFd);
            _lockFd = -1;
        }
    }

    LockingPiHal(const LockingPiHal &) = delete;
    LockingPiHal &operator=(const LockingPiHal &) = delete;

    /* True only if the process-shared SPI lock file was established. The daemon
     * must refuse to start a radio whose HAL is not lock-ready (fail closed). */
    bool spi_lock_ready() const { return _lockFd >= 0; }

    void spiBeginTransaction() override {
        /* Recursion guard: RadioLib does not nest transactions, but a depth
         * counter keeps a (hypothetical) nested begin/end pair from releasing
         * the lock early. Per-band SPI is serialized by radio_mutex, so the
         * counter is only ever touched by one thread per instance. */
        if (_depth++ == 0) {
            if (_lockFd < 0)
                fatal("SPI-Sperre nicht verfuegbar (fail-closed)");

            if (loraham_flock_acquire_ex(_lockFd, flock) != 0)
                fatal("flock(LOCK_EX) hart fehlgeschlagen");

            _held = true;
        }
    }

    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override {
        /* Hard invariant: never touch the shared SPI bus without the lock. */
        if (!_held)
            fatal("SPI-Transfer ohne gehaltene Sperre");

        PiHal::spiTransfer(out, len, in);
    }

    void spiEndTransaction() override {
        if (_depth > 0 && --_depth == 0) {
            _held = false;
            if (_lockFd >= 0)
                flock(_lockFd, LOCK_UN);
        }
    }

  private:
    [[noreturn]] static void fatal(const char *why) {
        fprintf(stderr,
                "[SPI] FATAL: %s - breche ab, um unsynchronisierten "
                "SPI-Zugriff zu verhindern\n", why);
        fflush(stderr);
        _exit(LORAHAM_EXIT_LOCK_ERROR);
    }

    void open_lock() {
        const char *dir = loraham_runtime_dir();
        char path[256];
        int n;
        int fd;

        /* Idempotent; in production the directory is pre-created root-owned by
         * tmpfiles.d. No insecure /tmp fallback: if the trusted path cannot be
         * used we fail closed (spi_lock_ready() stays false). */
        mkdir(dir, 0755);

        n = snprintf(path, sizeof(path), "%s/spi0.lock", dir);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            fprintf(stderr, "[SPI] Fehler: SPI-Sperrpfad zu lang\n");
            return;
        }

        /* O_NOFOLLOW refuses a symlink planted at the lock path. */
        fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0660);
        if (fd < 0) {
            fprintf(stderr,
                    "[SPI] Fehler: SPI-Sperrdatei %s nicht nutzbar: %s\n",
                    path, strerror(errno));
            return;
        }

        _lockFd = fd;
        fprintf(stderr, "[SPI] SPI-Sperrdatei: %s\n", path);
    }

    int _lockFd = -1;
    int _depth = 0;
    bool _held = false;
};

#endif
