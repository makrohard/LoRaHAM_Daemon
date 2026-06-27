/*
 * Unit tests for LockingPiHal -- the process-shared SPI transaction lock.
 *
 * These tests construct a LockingPiHal (which does NOT open SPI/GPIO; only
 * init() would) and exercise spiBeginTransaction()/spiEndTransaction(), then
 * observe the shared lock file through independent file descriptors and a child
 * process.  No radio hardware is required.
 */

#include "../locking_pihal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_ok = 0;
static int g_fail = 0;

static char g_tmpdir[256];

static void expect_int(const char *name, int actual, int expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %d, got %d\n", name, expected, actual);
    }
}

static const char *lock_path(void)
{
    static char path[320];
    const char *dir = getenv("LORAHAM_RUNTIME_DIR");

    if (!dir || !*dir)
        dir = "/run/loraham";

    snprintf(path, sizeof(path), "%s/spi0.lock", dir);
    return path;
}

/* Probe the shared lock file through a fresh, independent descriptor.
 * Returns 1 if the lock was free (and momentarily grabbed+released here),
 * 0 if it was held by someone else, -1 on error. */
static int probe_lock_free(void)
{
    int fd = open(lock_path(), O_CREAT | O_RDWR, 0660);
    int got;

    if (fd < 0)
        return -1;

    got = flock(fd, LOCK_EX | LOCK_NB);
    if (got == 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static void test_basic_lock_release(void)
{
    LockingPiHal hal(0);

    expect_int("free before begin", probe_lock_free(), 1);

    hal.spiBeginTransaction();
    expect_int("held during transaction", probe_lock_free(), 0);

    hal.spiEndTransaction();
    expect_int("free after end", probe_lock_free(), 1);
}

static void test_recursion_guard(void)
{
    LockingPiHal hal(0);

    hal.spiBeginTransaction();   /* depth 1 */
    hal.spiBeginTransaction();   /* depth 2 */
    expect_int("held at depth 2", probe_lock_free(), 0);

    hal.spiEndTransaction();     /* depth 1 -- must stay held */
    expect_int("still held at depth 1", probe_lock_free(), 0);

    hal.spiEndTransaction();     /* depth 0 -- now released */
    expect_int("released at depth 0", probe_lock_free(), 1);
}

static void test_two_instances_share_one_lock(void)
{
    LockingPiHal a(0);
    LockingPiHal b(0);

    a.spiBeginTransaction();
    /* An independent descriptor (the same view another instance/process has)
     * must see the lock held. */
    expect_int("instance A transaction excludes others", probe_lock_free(), 0);
    a.spiEndTransaction();

    b.spiBeginTransaction();
    expect_int("instance B transaction excludes others", probe_lock_free(), 0);
    b.spiEndTransaction();
    expect_int("free after both instances done", probe_lock_free(), 1);
}

/* The core guarantee: a *separate process* cannot enter an SPI transaction
 * while this process holds one. */
static void test_cross_process_exclusion(void)
{
    LockingPiHal hal(0);
    pid_t pid;
    int status;

    hal.spiBeginTransaction();

    pid = fork();
    if (pid == 0) {
        /* Child: own process, opens its own descriptor. Should see HELD. */
        _exit(probe_lock_free() == 0 ? 0 : 1);
    }
    waitpid(pid, &status, 0);
    expect_int("child process sees lock held",
               (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0, 1);

    hal.spiEndTransaction();

    pid = fork();
    if (pid == 0) {
        /* Child should now see FREE. */
        _exit(probe_lock_free() == 1 ? 0 : 1);
    }
    waitpid(pid, &status, 0);
    expect_int("child process sees lock free after end",
               (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0, 1);
}

int main(void)
{
    /* Isolate the lock file in a private temp dir. */
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/loraham-test-%d", (int)getpid());
    mkdir(g_tmpdir, 0755);
    setenv("LORAHAM_RUNTIME_DIR", g_tmpdir, 1);

    test_basic_lock_release();
    test_recursion_guard();
    test_two_instances_share_one_lock();
    test_cross_process_exclusion();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
