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
 *   exclusive, giving correct serialization between two separate per-band
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
/* SPI transactions are µs–ms; 2 s of contention on the shared bus lock can
 * only mean a wedged peer process. */
#define LORAHAM_SPI_LOCK_TIMEOUT_MS 2000L

class LockingPiHal : public PiHal {
  public:
    /* flock_fn is injectable for tests; production uses the real flock(). */
    LockingPiHal(uint8_t spiChannel, uint32_t spiSpeed = 2000000,
                 uint8_t spiDevice = 0, uint8_t gpioDevice = 0,
                 loraham_flock_fn flock_fn = flock)
      : PiHal(spiChannel, spiSpeed, spiDevice, gpioDevice),
        _ownSpiDevice(spiDevice), _ownSpiChannel(spiChannel),
        _ownSpiSpeed(spiSpeed), _flock(flock_fn) {
        open_lock();
    }

    ~LockingPiHal() override {
        if (_lockFd >= 0) {
            /* Best-effort at teardown; the kernel releases the lock on close. */
            _flock(_lockFd, LOCK_UN);
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

            /* Bounded (audit P1-3): a live-but-wedged peer must not block
             * this daemon forever. Expiry is fatal — systemd restarts a
             * dead process; it cannot see a silently hung one. */
            if (loraham_flock_acquire_ex_deadline(_lockFd, _flock,
                    LORAHAM_SPI_LOCK_TIMEOUT_MS) != 0) {
                if (errno == ETIMEDOUT)
                    fatal("SPI-Sperre nicht binnen Frist erhalten "
                          "(Peer verklemmt?)");
                fatal("flock(LOCK_EX) hart fehlgeschlagen");
            }

            _held = true;
        }
    }

    /* SPI ownership (audit P1-1): the base PiHal prints and SWALLOWS
     * lgSpiXfer/lgSpiOpen errors, so a RadioLib setter can report success
     * after the bus transfer failed. This HAL owns the SPI handle itself and
     * fails closed: an open failure leaves the handle invalid (transfers
     * fatal), a transfer failure is fatal — silent register corruption is
     * never an option. */
    void spiBegin() override {
        if (_ownSpiHandle >= 0)
            return;

        _ownSpiHandle = lgSpiOpen(_ownSpiDevice, _ownSpiChannel,
                                  _ownSpiSpeed, 0);
        if (_ownSpiHandle < 0)
            fprintf(stderr, "[SPI] lgSpiOpen fehlgeschlagen: %s\n",
                    lguErrorText(_ownSpiHandle));
    }

    void spiEnd() override {
        if (_ownSpiHandle >= 0) {
            lgSpiClose(_ownSpiHandle);
            _ownSpiHandle = -1;
        }
    }

    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override {
        /* Hard invariant: never touch the shared SPI bus without the lock. */
        if (!_held)
            fatal("SPI-Transfer ohne gehaltene Sperre");

        if (_ownSpiHandle < 0)
            fatal("SPI-Transfer ohne offenes SPI-Handle");

        int result = lgSpiXfer(_ownSpiHandle, (char *)out, (char *)in, len);
        if (result < 0) {
            fprintf(stderr, "[SPI] lgSpiXfer fehlgeschlagen: %s\n",
                    lguErrorText(result));
            fatal("SPI-Transfer fehlgeschlagen (Bus-Fehler)");
        }
    }

    void spiEndTransaction() override {
        if (_depth > 0 && --_depth == 0) {
            /* Bookkeeping first so state stays consistent on the fatal path. */
            _held = false;
            if (_lockFd >= 0 &&
                loraham_flock_release(_lockFd, _flock) != 0) {
                /* A failed unlock would leave the kernel lock held while we
                 * believe it released -- that could wedge the peer band. Treat
                 * it as fatal: exit via the lock-error path so process teardown
                 * closes the fd and the kernel releases the lock. */
                fatal("flock(LOCK_UN) hart fehlgeschlagen");
            }
        }
    }

  private:
    /* Runtime fatal (audit item 4): every fatal in this HAL fires AFTER
     * operation began (transfer without lock/handle, bus error, wedged-peer
     * timeout, hard un/lock failure). Exit 5 — distinct from the startup
     * lock-infrastructure code 4 — so systemd's Restart=on-failure may
     * restart, while codes 3/4 stay non-restartable. */
    [[noreturn]] static void fatal(const char *why) {
        fprintf(stderr,
                "[SPI] FATAL: %s - breche ab, um unsynchronisierten "
                "SPI-Zugriff zu verhindern\n", why);
        fflush(stderr);
        _exit(LORAHAM_EXIT_RUNTIME_SPI_ERROR);
    }

    int _ownSpiHandle = -1;
    uint8_t _ownSpiDevice;
    uint8_t _ownSpiChannel;
    uint32_t _ownSpiSpeed;

    void open_lock() {
        /* Validate the trusted lock directory, then create spi0.lock relative to
         * it. No insecure /tmp fallback and no silent creation of an untrusted
         * production directory: if the trusted path cannot be used we fail closed
         * (spi_lock_ready() stays false). */
        int dirfd = loraham_open_runtime_dir();
        if (dirfd < 0)
            return;

        _lockFd = loraham_open_lock_file_at(dirfd, "spi0.lock");
        close(dirfd);

        if (_lockFd >= 0)
            fprintf(stderr, "[SPI] SPI-Sperrdatei: %s/spi0.lock\n",
                    loraham_runtime_dir());
    }

    loraham_flock_fn _flock;
    int _lockFd = -1;
    int _depth = 0;
    bool _held = false;
};

#endif
