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
 * usual startup failure; what is checked is only that the library was never
 * opened on the way.
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

    sh("rm -rf '" + tmp + "'");
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
