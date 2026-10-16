/*
 * Only `--hw meshstick-1262` ever loads the Meshstick library.
 *
 * The REAL daemon binary is copied into a fresh directory whose lib/ holds a
 * marker library in place of libloraham-ch341.so: its constructor writes a
 * file the moment anything dlopen()s it. Every existing preset is then
 * started on each band; none may touch the marker. Two controls keep that
 * from passing vacuously: meshstick-1262 with the marker library DOES write
 * the marker (and is refused, the marker exports nothing), and
 * meshstick-1262 without any library is refused with exit 4 and the reason.
 *
 * The starts run without radio hardware, so the existing presets end at their
 * usual startup failure; what is checked is that the library was never opened
 * on the way, and that each Pi preset still reports its SPI chip select.
 *
 * A third run gives meshstick-1262 a fake library the loader accepts (every
 * symbol, the lock's commit; its open finds no stick): the Pi's "[SPI] chip
 * select" line must not appear for a USB radio. (Where the start ends after
 * that depends on the lane's lock and socket directories, so it is not checked.)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_ok = 0;
static int g_fail = 0;

static void expect(const char *name, bool cond)
{
    if (cond) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
}

static int sh(const std::string &cmd)
{
    int rc = system(cmd.c_str());
    return (rc != -1 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
}

static bool exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

/* Run `bin --radio <band> --hw <preset>`, output into `log`, at most 20 s.
 * Returns the exit code (-1 when killed). */
static int run_daemon(const std::string &bin, const char *band, const char *preset,
                      const std::string &log, const std::string &marker)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        FILE *out = freopen(log.c_str(), "w", stdout);
        (void)out;
        dup2(STDOUT_FILENO, STDERR_FILENO);
        setenv("LORAHAM_TEST_DLOPEN_MARKER", marker.c_str(), 1);
        execl(bin.c_str(), bin.c_str(), "--radio", band, "--hw", preset, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    for (int i = 0; i < 200; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

int main(int argc, char *argv[])
{
    const char *daemon = nullptr;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--bin") == 0)
            daemon = argv[i + 1];
    if (!daemon) {
        printf("[FAIL] usage: %s --bin <loraham_daemon>\n", argv[0]);
        return 1;
    }

    char tmpl[] = "/tmp/loraham-ch341-dlopen.XXXXXX";
    if (!mkdtemp(tmpl)) {
        printf("[FAIL] mkdtemp\n");
        return 1;
    }
    std::string tmp = tmpl;
    std::string bin = tmp + "/bundle/loraham_daemon";
    std::string so = tmp + "/bundle/lib/libloraham-ch341.so";
    std::string marker = tmp + "/marker";

    sh("mkdir -p '" + tmp + "/bundle/lib' && cp '" + std::string(daemon) + "' '" + bin + "'");

    /* The marker library: records a dlopen(), exports nothing the loader wants. */
    {
        FILE *f = fopen((tmp + "/marker.c").c_str(), "w");
        fputs("#include <stdio.h>\n#include <stdlib.h>\n"
              "__attribute__((constructor)) static void mark(void) {\n"
              "  const char *p = getenv(\"LORAHAM_TEST_DLOPEN_MARKER\");\n"
              "  FILE *f = p ? fopen(p, \"a\") : NULL;\n"
              "  if (f) { fputs(\"opened\\n\", f); fclose(f); }\n"
              "}\n", f);
        fclose(f);
    }
    expect("marker library built",
           sh("cc -shared -fPIC -o '" + so + "' '" + tmp + "/marker.c'") == 0);

    static const char *presets[] = { "loraham", "uputronics-ce0", "uputronics-ce1",
                                     "waveshare-sx1262" };
    static const char *bands[] = { "433", "868" };

    for (const char *preset : presets) {
        for (const char *band : bands) {
            std::string log = tmp + "/log-" + preset + "-" + band + ".txt";
            int rc = run_daemon(bin, band, preset, log, marker);
            std::string t = std::string(preset) + " --radio " + band +
                            ": the Meshstick library is never opened (exit " +
                            std::to_string(rc) + ")";
            expect(t.c_str(), !exists(marker));
            t = std::string(preset) + " --radio " + band + ": reports its SPI chip select";
            expect(t.c_str(), sh("grep -q '\\[SPI\\] chip select' '" + log + "'") == 0);
        }
    }

    /* Control 1: the Meshstick preset does open it, and refuses the marker. */
    {
        std::string log = tmp + "/log-meshstick-marker.txt";
        int rc = run_daemon(bin, "868", "meshstick-1262", log, marker);
        expect("control: meshstick-1262 opens the library in lib/", exists(marker));
        expect("control: a library without the adapter symbols -> exit 4", rc == 4);
        expect("control: reason is the missing symbol",
               sh("grep -q 'CH341\\] refused: missing symbol' '" + log + "'") == 0);
    }

    /* Control 2: no library at all -> exit 4 with the reason, nothing opened. */
    {
        unlink(marker.c_str());
        unlink(so.c_str());
        std::string log = tmp + "/log-meshstick-none.txt";
        int rc = run_daemon(bin, "868", "meshstick-1262", log, marker);
        expect("no library: exit 4", rc == 4);
        expect("no library: reason names the missing library",
               sh("grep -q 'CH341\\] refused: Meshstick library not found' '" + log + "'") == 0);
        sh("cat '" + log + "'");
    }

    /* Control 3: an acceptable library (the lock's commit, every symbol; open
     * finds no stick). The USB radio prints no Pi chip-select line. */
    {
        std::string exe_dir(daemon);
        exe_dir = exe_dir.substr(0, exe_dir.rfind('/'));
        std::string lockf = exe_dir + "/../.github/ci/libpinedio.lock";
        std::string pin_file = tmp + "/pin.txt";
        sh("sed -n 's/^PINEDIO_COMMIT=//p' '" + lockf + "' > '" + pin_file + "'");
        char pin[64] = {0};
        FILE *pf = fopen(pin_file.c_str(), "r");
        if (pf) {
            if (!fgets(pin, sizeof(pin), pf))
                pin[0] = '\0';
            fclose(pf);
        }
        pin[strcspn(pin, "\n")] = '\0';
        expect("control 3: the lock's commit read", strlen(pin) == 40);

        FILE *f = fopen((tmp + "/good.c").c_str(), "w");
        fprintf(f, "#include <stdint.h>\ntypedef struct loraham_ch341 loraham_ch341;\n"
                   "const char *loraham_ch341_pin(void) { return \"%s\"; }\n"
                   "loraham_ch341 *loraham_ch341_open(uint16_t v, uint16_t p, const char *s) { (void)v; (void)p; (void)s; return 0; }\n"
                   "void loraham_ch341_close(loraham_ch341 *d) { (void)d; }\n"
                   "int loraham_ch341_transceive(loraham_ch341 *d, const uint8_t *o, uint8_t *i, uint32_t n) { (void)d; (void)o; (void)i; (void)n; return -1; }\n"
                   "int loraham_ch341_pin_mode(loraham_ch341 *d, uint32_t p, int o) { (void)d; (void)p; (void)o; return -1; }\n"
                   "int loraham_ch341_digital_write(loraham_ch341 *d, uint32_t p, int l) { (void)d; (void)p; (void)l; return -1; }\n"
                   "int loraham_ch341_digital_read(loraham_ch341 *d, uint32_t p) { (void)d; (void)p; return -1; }\n"
                   "int loraham_ch341_attach_interrupt(loraham_ch341 *d, uint32_t p, uint32_t m, void (*c)(void)) { (void)d; (void)p; (void)m; (void)c; return -1; }\n"
                   "int loraham_ch341_detach_interrupt(loraham_ch341 *d, uint32_t p) { (void)d; (void)p; return -1; }\n"
                   "int loraham_ch341_in_error(loraham_ch341 *d) { (void)d; return 1; }\n", pin);
        fclose(f);
        expect("control 3: acceptable fake library built",
               sh("cc -shared -fPIC -o '" + so + "' '" + tmp + "/good.c'") == 0);
        std::string log = tmp + "/log-meshstick-good.txt";
        run_daemon(bin, "868", "meshstick-1262", log, marker);
        expect("control 3: the library is accepted",
               sh("grep -q 'CH341\\] Meshstick library loaded' '" + log + "'") == 0);
        expect("control 3: no Pi \"[SPI] chip select\" line for the USB radio",
               sh("grep -q '\\[SPI\\] chip select' '" + log + "'") != 0);
        sh("cat '" + log + "'");
    }

    sh("rm -rf '" + tmp + "'");
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
