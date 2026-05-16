#include "../data_tx.h"

#include <stdio.h>
#include <string.h>

/*
 * DATA TX unit tests.
 *
 * This locks the LoRa packet chunk size before DATA TX handling is moved
 * out of the daemon file.
 */

static int g_ok = 0;
static int g_fail = 0;

/* --- Test helpers --- */

static void expect_size(const char *name, size_t actual, size_t expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %zu, got %zu\n", name, expected, actual);
    }
}

/* --- Chunking behavior --- */

static void test_chunk_size(void)
{
    expect_size("zero remaining", data_tx_chunk_size(0), 0);
    expect_size("one byte", data_tx_chunk_size(1), 1);
    expect_size("max exact", data_tx_chunk_size(255), 255);
    expect_size("max plus one", data_tx_chunk_size(256), 255);
    expect_size("large input", data_tx_chunk_size(2048), 255);
}

/* --- CLI parsing and test sequence --- */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bin") == 0) {
            if (i + 1 >= argc) {
                printf("Usage: %s [--bin ignored]\n", argv[0]);
                return 2;
            }
            i++;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--bin ignored]\n", argv[0]);
            return 0;
        } else {
            printf("Usage: %s [--bin ignored]\n", argv[0]);
            return 2;
        }
    }

    test_chunk_size();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);

    return g_fail ? 1 : 0;
}
