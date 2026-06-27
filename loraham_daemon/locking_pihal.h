#ifndef LORAHAM_LOCKING_PIHAL_H
#define LORAHAM_LOCKING_PIHAL_H

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hal/RPi/PiHal.h"

/*
 * PiHal that serializes every SPI transaction across processes (and across
 * bands within one process) using an advisory flock() on a shared lock file.
 *
 * Why this is the correct interception point:
 *   RadioLib's Module brackets each transfer as
 *       spiBeginTransaction(); CS-low; spiTransfer(); CS-high; spiEndTransaction();
 *   (RadioLib/src/Module.cpp).  Taking the lock in spiBeginTransaction() and
 *   releasing it in spiEndTransaction() therefore protects the entire
 *   CS-low -> transfer -> CS-high window: exactly one complete SPI transaction,
 *   never the whole daemon lifetime.  The bus is free between transactions, so
 *   complete transactions to the two radios (each on its own CS line) may
 *   interleave -- which is exactly what a shared SPI bus permits.
 *
 * Why per-instance file descriptors:
 *   Each radio constructs its own LockingPiHal, so each holds its own open file
 *   description on the same lock file.  flock() on distinct open file
 *   descriptions is mutually exclusive even within one process, giving correct
 *   serialization both between the two bands of a single --radio both process
 *   and between two separate per-band daemons sharing /dev/spidev0.0.
 *
 * Crash safety:
 *   flock() locks are released by the kernel when the owning descriptor is
 *   closed, which happens automatically on process death -- so a crashed daemon
 *   never wedges the bus and there is no stale lock state to clean up.
 *
 * Lock-ordering note:
 *   Callers always hold the per-band radio_mutex (in-process) before any SPI
 *   call, and the flock here is the lowest-level lock taken last, so there is
 *   no inverse acquisition path and no deadlock.
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

    void spiBeginTransaction() override {
        /* Recursion guard: RadioLib does not nest transactions, but a depth
         * counter keeps a (hypothetical) nested begin/end pair from releasing
         * the lock early.  Per-band SPI is serialized by radio_mutex, so the
         * counter is only ever touched by one thread per instance. */
        if (_lockFd >= 0 && _depth++ == 0) {
            while (flock(_lockFd, LOCK_EX) < 0) {
                if (errno == EINTR)
                    continue;
                /* Locking failed unexpectedly; do not spin forever. */
                break;
            }
        }
    }

    void spiEndTransaction() override {
        if (_lockFd >= 0 && _depth > 0 && --_depth == 0)
            flock(_lockFd, LOCK_UN);
    }

  private:
    void open_lock() {
        const char *dir = getenv("LORAHAM_RUNTIME_DIR");

        if (!dir || !*dir)
            dir = "/run/loraham";

        if (try_open_in(dir))
            return;

        /* Fall back to an always-writable location so two non-root instances
         * (dev/test runs) still share one lock file deterministically. */
        if (try_open_in("/tmp/loraham"))
            return;

        fprintf(stderr,
                "[SPI] WARNUNG: SPI-Sperrdatei konnte nicht angelegt werden – "
                "prozessübergreifende SPI-Serialisierung ist DEAKTIVIERT\n");
    }

    bool try_open_in(const char *dir) {
        char path[256];
        int n;
        int fd;

        /* mkdir is idempotent; an existing directory is fine. */
        mkdir(dir, 0755);

        n = snprintf(path, sizeof(path), "%s/spi0.lock", dir);
        if (n <= 0 || (size_t)n >= sizeof(path))
            return false;

        fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0660);
        if (fd < 0)
            return false;

        _lockFd = fd;
        fprintf(stderr, "[SPI] SPI-Sperrdatei: %s\n", path);
        return true;
    }

    int _lockFd = -1;
    int _depth = 0;
};

#endif
