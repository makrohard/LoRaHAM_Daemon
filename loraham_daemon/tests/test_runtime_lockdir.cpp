/*
 * Unit tests for the trusted lock-directory / lock-file validation helpers in
 * loraham_runtime.h (M4-P0-2). No radio hardware required.
 *
 * Run as a non-root user: the root-owner requirement is exercised by asking for
 * require_root on a user-owned directory (which must be rejected).
 */

#include "../loraham_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_ok = 0;
static int g_fail = 0;

static char g_base[200];

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

/* Returns 1 if the dir validated (fd >= 0), 0 if rejected. Closes any fd. */
static int dir_accepts(const char *dir, int require_root)
{
    int fd = loraham_open_lock_dir(dir, require_root);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    return 0;
}

static void path_in(const char *leaf, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", g_base, leaf);
}

static void test_missing_dir(void)
{
    char p[256];
    path_in("does-not-exist", p, sizeof(p));
    expect_int("missing dir rejected", dir_accepts(p, 0), 0);
}

static void test_symlink_dir(void)
{
    char real[256], link[256];
    path_in("real-dir", real, sizeof(real));
    path_in("link-dir", link, sizeof(link));
    mkdir(real, 0700);
    unlink(link);
    symlink(real, link);
    /* O_NOFOLLOW must reject the symlinked directory path. */
    expect_int("symlinked dir rejected", dir_accepts(link, 0), 0);
}

static void test_non_directory(void)
{
    char p[256];
    int fd;
    path_in("a-file", p, sizeof(p));
    fd = open(p, O_CREAT | O_RDWR, 0600);
    if (fd >= 0)
        close(fd);
    expect_int("non-directory rejected", dir_accepts(p, 0), 0);
}

static void test_group_world_writable(void)
{
    char g[256], w[256];

    path_in("grp-writable", g, sizeof(g));
    mkdir(g, 0700);
    chmod(g, 0770);                         /* group-writable */
    expect_int("group-writable dir rejected", dir_accepts(g, 0), 0);

    path_in("world-writable", w, sizeof(w));
    mkdir(w, 0700);
    chmod(w, 0707);                         /* world-writable */
    expect_int("world-writable dir rejected", dir_accepts(w, 0), 0);
}

static void test_non_root_owner(void)
{
    char p[256];
    path_in("user-owned", p, sizeof(p));
    mkdir(p, 0700);
    /* Owned by the (non-root) test user; require_root must reject it. */
    expect_int("non-root owner rejected when require_root", dir_accepts(p, 1), 0);
    /* ...but accepted in relaxed (dev/override) mode. */
    expect_int("user-owned accepted when require_root=0", dir_accepts(p, 0), 1);
}

static void test_lock_file_regular(void)
{
    char dir[256];
    int dirfd, fd;
    path_in("clean-dir", dir, sizeof(dir));
    mkdir(dir, 0700);

    dirfd = loraham_open_lock_dir(dir, 0);
    expect_int("clean dir accepted", dirfd >= 0 ? 1 : 0, 1);

    fd = loraham_open_lock_file_at(dirfd, "spi0.lock");
    expect_int("regular lock file accepted", fd >= 0 ? 1 : 0, 1);
    if (fd >= 0)
        close(fd);
    if (dirfd >= 0)
        close(dirfd);
}

static void test_lock_file_symlink(void)
{
    char dir[256], target[256];
    int dirfd, fd;
    path_in("symfile-dir", dir, sizeof(dir));
    mkdir(dir, 0700);
    path_in("symfile-dir/target", target, sizeof(target));

    dirfd = loraham_open_lock_dir(dir, 0);
    /* Plant a symlink where the lock file would be created. */
    symlinkat(target, dirfd, "spi0.lock");

    fd = loraham_open_lock_file_at(dirfd, "spi0.lock");
    expect_int("symlinked lock file rejected", fd >= 0 ? 1 : 0, 0);
    if (fd >= 0)
        close(fd);
    if (dirfd >= 0)
        close(dirfd);
}

static void test_lock_file_non_regular(void)
{
    char dir[256], fifo[256];
    int dirfd, fd;
    path_in("fifo-dir", dir, sizeof(dir));
    mkdir(dir, 0700);
    path_in("fifo-dir/spi0.lock", fifo, sizeof(fifo));
    mkfifo(fifo, 0600);

    dirfd = loraham_open_lock_dir(dir, 0);
    fd = loraham_open_lock_file_at(dirfd, "spi0.lock");
    expect_int("non-regular lock file (fifo) rejected", fd >= 0 ? 1 : 0, 0);
    if (fd >= 0)
        close(fd);
    if (dirfd >= 0)
        close(dirfd);
}

/* Override mode creates a private 0700 dir; production path is not silently
 * created here (open of a missing dir never creates it). */
static void test_override_behaviour(void)
{
    char dir[256];
    struct stat st;
    int fd;

    path_in("override-created", dir, sizeof(dir));
    setenv("LORAHAM_RUNTIME_DIR", dir, 1);

    expect_int("override detected", loraham_runtime_dir_is_override(), 1);

    fd = loraham_open_runtime_dir();
    expect_int("override creates+accepts dir", fd >= 0 ? 1 : 0, 1);
    if (fd >= 0)
        close(fd);

    if (stat(dir, &st) == 0)
        expect_int("override dir is 0700", (int)(st.st_mode & 07777), 0700);
    else
        expect_int("override dir exists", 0, 1);

    unsetenv("LORAHAM_RUNTIME_DIR");
    expect_int("no override after unset", loraham_runtime_dir_is_override(), 0);

    /* loraham_open_lock_dir never creates a missing directory. */
    {
        char p[256];
        path_in("never-created", p, sizeof(p));
        (void)loraham_open_lock_dir(p, 0);
        expect_int("dir-open does not create", access(p, F_OK) == 0 ? 1 : 0, 0);
    }
}

int main(void)
{
    snprintf(g_base, sizeof(g_base), "/tmp/loraham-lockdir-%d", (int)getpid());
    mkdir(g_base, 0700);

    test_missing_dir();
    test_symlink_dir();
    test_non_directory();
    test_group_world_writable();
    test_non_root_owner();
    test_lock_file_regular();
    test_lock_file_symlink();
    test_lock_file_non_regular();
    test_override_behaviour();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
