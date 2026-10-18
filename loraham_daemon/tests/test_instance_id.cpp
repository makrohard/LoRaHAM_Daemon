/*
 * --id <radio-id> (daemon_band.h): the accepted format, the names it gives the
 * sockets, the instance lock and the -d log, and the socket path length check.
 *
 * Unit part: daemon_band.cpp directly. Binary part (--bin): the real daemon
 * refuses a bad --id with exit 1 and an over-long socket path with exit 4,
 * both before any hardware is touched.
 */

#include "../daemon_band.h"
#include "../daemon_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

static int g_ok = 0;
static int g_fail = 0;

static void expect(const char *name, bool ok)
{
    if (ok) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
}

static void expect_str(const char *name, const char *actual, const char *expected)
{
    bool ok = actual && expected && strcmp(actual, expected) == 0;
    if (ok) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected '%s', got '%s'\n", name,
               expected ? expected : "(null)", actual ? actual : "(null)");
    }
}

/* T17: the format. */
static void test_id_format(void)
{
    const char *good[] = {"r", "r3", "ms1", "a-b", "abcdefghijklmnop", "z9-9"};
    const char *bad[] = {"", "3r", "-r", "R3", "rX", "abcdefghijklmnopq", "r/3", "r.3",
                         "r 3", "r_3"};

    for (const char *g : good) {
        std::string n = std::string("valid id: ") + g;
        expect(n.c_str(), daemon_instance_id_valid(g));
    }
    for (const char *b : bad) {
        std::string n = std::string("refused id: '") + b + "'";
        expect(n.c_str(), !daemon_instance_id_valid(b));
    }
    expect("NULL id refused", !daemon_instance_id_valid(NULL));
    expect("set refuses a bad id", !daemon_set_instance_id("3r"));
    expect("a refused set leaves no id", daemon_instance_id() == NULL);
}

/* T18: names with and without --id, both bands, and the -d log. */
static void test_names(void)
{
    unsetenv("LORAHAM_SOCKET_DIR");
    daemon_instance_id_reset();

    daemon_band_resolve(RADIO_BAND_868);
    expect_str("868 without id: data", daemon_band()->data_socket, DATA868_SOCKET);
    expect_str("868 without id: framed", daemon_band()->framed_socket, DATA868_FRAMED_SOCKET);
    expect_str("868 without id: conf", daemon_band()->conf_socket, CONF868_SOCKET);
    expect_str("without id: -d log", daemon_band_daemon_log_path(), "/tmp/lora_daemon.log");
    daemon_band_resolve(RADIO_BAND_433);
    expect_str("433 without id: data", daemon_band()->data_socket, DATA433_SOCKET);
    expect_str("433 without id: conf", daemon_band()->conf_socket, CONF433_SOCKET);
    expect_str("the literal 1.2.1 names stay", DATA868_SOCKET, "/run/loraham/lora868.sock");
    expect_str("the literal 1.2.1 framed name stays", DATA868_FRAMED_SOCKET,
               "/run/loraham/lora868f.sock");
    expect_str("the literal 1.2.1 conf name stays", CONF868_SOCKET,
               "/run/loraham/loraconf868.sock");

    expect("set r3", daemon_set_instance_id("r3"));
    for (RadioBand_t b : {RADIO_BAND_433, RADIO_BAND_868}) {
        daemon_band_resolve(b);
        expect_str("with id: data", daemon_band()->data_socket, "/run/loraham/lora-r3.sock");
        expect_str("with id: framed", daemon_band()->framed_socket, "/run/loraham/loraf-r3.sock");
        expect_str("with id: conf", daemon_band()->conf_socket, "/run/loraham/loraconf-r3.sock");
    }
    expect_str("with id: -d log", daemon_band_daemon_log_path(), "/tmp/lora_daemon-r3.log");

    setenv("LORAHAM_SOCKET_DIR", "/tmp/x", 1);
    daemon_band_resolve(RADIO_BAND_868);
    expect_str("with id + dir: data", daemon_band()->data_socket, "/tmp/x/lora-r3.sock");
    unsetenv("LORAHAM_SOCKET_DIR");

    /* r3 and r3f: all six names distinct (the framed name of one must never be
     * the raw name of the other: setup_unix_socket() unlinks before bind). */
    std::string names[6];
    daemon_band_resolve(RADIO_BAND_868);
    names[0] = daemon_band()->data_socket;
    names[1] = daemon_band()->framed_socket;
    names[2] = daemon_band()->conf_socket;
    expect("set r3f", daemon_set_instance_id("r3f"));
    daemon_band_resolve(RADIO_BAND_868);
    names[3] = daemon_band()->data_socket;
    names[4] = daemon_band()->framed_socket;
    names[5] = daemon_band()->conf_socket;
    bool distinct = true;
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if (names[i] == names[j])
                distinct = false;
    expect("r3 and r3f: six distinct socket names", distinct);
    daemon_instance_id_reset();
}

/* T20: the whole path in bytes, before any copy, with and without --id. */
static void test_path_length(void)
{
    char why[400];
    /* "/" + "loraconf868.sock" = 17 bytes after the dir; 107 - 17 = 90. */
    std::string dir90 = "/" + std::string(89, 'd');
    std::string dir91 = dir90 + "d";

    daemon_instance_id_reset();
    setenv("LORAHAM_SOCKET_DIR", dir90.c_str(), 1);
    daemon_band_resolve(RADIO_BAND_868);
    expect("without id: a 107-byte conf path is accepted", daemon_band_socket_paths_ok(why, sizeof(why)));
    expect("without id: the 107-byte path is whole", strlen(daemon_band()->conf_socket) == 107);

    setenv("LORAHAM_SOCKET_DIR", dir91.c_str(), 1);
    daemon_band_resolve(RADIO_BAND_868);
    expect("without id: a 108-byte conf path is refused", !daemon_band_socket_paths_ok(why, sizeof(why)));
    expect("the reason names the length", strstr(why, "108 bytes") != NULL);
    expect("nothing truncated is handed out", daemon_band()->conf_socket[0] == '\0');

    /* With a 16-character id: "loraconf-" + 16 + ".sock" = 30, +1 slash -> dir 76. */
    expect("set a 16-character id", daemon_set_instance_id("abcdefghijklmnop"));
    std::string dir76 = "/" + std::string(75, 'e');
    setenv("LORAHAM_SOCKET_DIR", dir76.c_str(), 1);
    daemon_band_resolve(RADIO_BAND_868);
    expect("with id: a 76-byte directory fits", daemon_band_socket_paths_ok(why, sizeof(why)));
    setenv("LORAHAM_SOCKET_DIR", (dir76 + "e").c_str(), 1);
    daemon_band_resolve(RADIO_BAND_868);
    expect("with id: a 77-byte directory is refused", !daemon_band_socket_paths_ok(why, sizeof(why)));

    /* Bytes, not characters: "ä" is two bytes in UTF-8. */
    daemon_instance_id_reset();
    std::string mb = "/" + std::string(87, 'm') + "\xc3\xa4";   /* 90 bytes, 89 characters */
    setenv("LORAHAM_SOCKET_DIR", mb.c_str(), 1);
    daemon_band_resolve(RADIO_BAND_868);
    expect("multibyte: 90 bytes of directory fit exactly", daemon_band_socket_paths_ok(why, sizeof(why)));
    setenv("LORAHAM_SOCKET_DIR", (mb + "\xc3\xa4").c_str(), 1);   /* 92 bytes, 90 characters */
    daemon_band_resolve(RADIO_BAND_868);
    expect("multibyte: 92 bytes refused although 90 characters", !daemon_band_socket_paths_ok(why, sizeof(why)));

    unsetenv("LORAHAM_SOCKET_DIR");
    daemon_band_resolve(RADIO_BAND_868);
    expect("default paths pass", daemon_band_socket_paths_ok(why, sizeof(why)));
}

static int run(const std::string &cmd)
{
    int rc = system(cmd.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* The binary: exit 1 on a bad --id, exit 4 on an over-long socket path. */
static void test_binary(const char *bin)
{
    char tmpl[] = "/tmp/loraham-id-test.XXXXXX";
    if (!mkdtemp(tmpl)) {
        expect("mkdtemp", false);
        return;
    }
    std::string tmp(tmpl);
    std::string log = tmp + "/out.txt";
    std::string base = std::string("LORAHAM_RUNTIME_DIR='") + tmp + "/lock' '" + bin + "' --radio 868 --hw loraham";

    int rc = run(base + " --id 3r >'" + log + "' 2>&1");
    expect("binary: a bad --id exits 1", rc == 1);
    expect("binary: the reason names the format",
           run("grep -q 'invalid --id (1-16 of a-z 0-9 -, starting with a letter): 3r' '" + log + "'") == 0);

    std::string longdir = tmp + "/" + std::string(100, 'l');
    rc = run("LORAHAM_SOCKET_DIR='" + longdir + "' " + base + " --id r3 >'" + log + "' 2>&1");
    expect("binary: an over-long socket path exits 4", rc == 4);
    expect("binary: the refusal names the path",
           run("grep -q 'SOCKET\\] refused: socket path too long' '" + log + "'") == 0);

    /* core r16 F1: a diagnostic longer than 320 bytes is printed whole: the
     * path's tail and "(exit 4)." on the line (no truncation by the caller) */
    std::string verylong = tmp + "/" + std::string(200, 'v') + "/" + std::string(200, 'w') + "TAIL";
    rc = run("LORAHAM_SOCKET_DIR='" + verylong + "' " + base + " --id r3 >'" + log + "' 2>&1");
    expect("binary: a >320-byte refusal: exits 4", rc == 4);
    expect("binary: a >320-byte refusal: the line carries the path's tail and \"(exit 4).\"",
           run("grep -q 'wTAIL/lora[a-z]*-r3\\.sock (exit 4)\\.$' '" + log + "'") == 0);

    run("rm -rf '" + tmp + "'");
}

int main(int argc, char *argv[])
{
    const char *bin = nullptr;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--bin") == 0)
            bin = argv[i + 1];

    test_id_format();
    test_names();
    test_path_length();
    if (bin)
        test_binary(bin);
    else
        printf("[SKIP] binary part: no --bin\n");

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail == 0 ? 0 : 1;
}
