#include "../daemon_gpio_lock.h"
#include "../loraham_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Cross-process GPIO ownership tests (audit P1-1) ----------------------- */
/*
 * Uses a private LORAHAM_RUNTIME_DIR. flock is per open-file-description, so
 * a second open+flock on the same lock file conflicts even within one
 * process — that models the second band process.
 */

static int g_ok = 0;
static int g_fail = 0;

static char g_dir[128];

static void expect_int(const char *name, long actual, long expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %ld, got %ld\n", name, expected, actual);
    }
}

static int probe_pin_free(int pin)
{
    char path[192];
    int fd;
    int rc;

    snprintf(path, sizeof(path), "%s/gpio%d.lock", g_dir, pin);
    fd = open(path, O_RDWR | O_CREAT, 0660);
    if (fd < 0)
        return -1;

    rc = flock(fd, LOCK_EX | LOCK_NB);
    if (rc == 0)
        flock(fd, LOCK_UN);
    close(fd);

    return rc == 0 ? 1 : 0;
}

static void test_acquire_and_conflict(void)
{
    const int pins[] = { 13, 5, 24, 5, -1 }; /* unsorted, dup, NC */

    expect_int("acquire succeeds",
               daemon_gpio_locks_acquire(pins, 5) ? 1 : 0, 1);
    expect_int("three distinct pins held",
               (long)daemon_gpio_locks_held(), 3);
    expect_int("pin 5 is exclusively held", probe_pin_free(5), 0);
    expect_int("pin 13 is exclusively held", probe_pin_free(13), 0);
    expect_int("unrelated pin stays free", probe_pin_free(19), 1);

    daemon_gpio_locks_release();
    expect_int("release frees pin 5", probe_pin_free(5), 1);
    expect_int("release clears held count",
               (long)daemon_gpio_locks_held(), 0);
}

static void test_conflict_fails_closed(void)
{
    char path[192];
    const int mine[] = { 6 };
    const int theirs[] = { 12, 6 };

    /* "Other process": hold gpio6.lock via a raw descriptor. */
    snprintf(path, sizeof(path), "%s/gpio6.lock", g_dir);
    int fd = open(path, O_RDWR | O_CREAT, 0660);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        g_fail++;
        printf("[FAIL] conflict setup\n");
        if (fd >= 0)
            close(fd);
        return;
    }
    (void)mine;

    expect_int("conflicting acquire fails",
               daemon_gpio_locks_acquire(theirs, 2) ? 1 : 0, 0);
    expect_int("failed acquire holds nothing",
               (long)daemon_gpio_locks_held(), 0);
    /* Pin 12 must have been rolled back, not leaked. */
    expect_int("non-conflicting pin rolled back", probe_pin_free(12), 1);

    flock(fd, LOCK_UN);
    close(fd);

    expect_int("acquire succeeds after peer release",
               daemon_gpio_locks_acquire(theirs, 2) ? 1 : 0, 1);
    daemon_gpio_locks_release();
}

int main(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/loraham-gpiolock-%d", (int)getpid());
    if (mkdir(g_dir, 0700) != 0) {
        printf("[FAIL] test dir setup\n");
        printf("\nSummary: ok=0 fail=1\n");
        return 1;
    }
    setenv("LORAHAM_RUNTIME_DIR", g_dir, 1);

    test_acquire_and_conflict();
    test_conflict_fails_closed();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
