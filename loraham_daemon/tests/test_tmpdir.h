/*
 * A test's own directory under TMPDIR.
 *
 * run_tests.sh points TMPDIR at a directory of its own and removes it at exit,
 * so whatever a test leaves there (the lock files its daemons create, a test
 * killed by its timeout) goes with the run. Without TMPDIR it is /tmp, as
 * before.
 *
 * The path is at most TEST_TMP_DIR_MAX characters: it fits the 128-byte
 * buffers the tests keep it in, and the buffers they append lock-file names in
 * are larger. A longer TMPDIR fails the test loudly rather than truncating a
 * path (run_tests.sh's own directory under /tmp gives about 50).
 * No socket is created under it: the tests' sockets stay in LORAHAM_SOCKET_DIR.
 */
#ifndef LORAHAM_TEST_TMPDIR_H
#define LORAHAM_TEST_TMPDIR_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_TMP_DIR_MAX 120

/* Writes "<TMPDIR or /tmp>/<name>-<pid>" to buf (size >= TEST_TMP_DIR_MAX + 1). */
static inline void test_tmp_dir(char *buf, size_t size, const char *name)
{
    const char *base = getenv("TMPDIR");
    int n;

    if (!base || !*base)
        base = "/tmp";
    n = snprintf(buf, size, "%s/%s-%d", base, name, (int)getpid());
    if (n < 0 || n > TEST_TMP_DIR_MAX || (size_t)n >= size) {
        printf("[FAIL] test dir path longer than %d characters (TMPDIR=%s)\n", TEST_TMP_DIR_MAX, base);
        printf("\nSummary: ok=0 fail=1\n");
        exit(1);
    }
}

#endif
