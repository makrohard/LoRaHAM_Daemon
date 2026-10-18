/*
 * build.sh and the Meshstick library: the pine64 tree is attested before
 * anything is compiled from it.
 *
 * build.sh builds lib/libloraham-ch341.so only from a git checkout that is
 * exactly the commit in the lock and unmodified, and embeds the MEASURED
 * commit. The runtime pin check in the daemon is worth only as much as this
 * measurement, so every way to hand build.sh a different source is tried
 * here: another commit, local edits, an extra file, a plain directory, a
 * directory inside some other repository, an unusable PINEDIO_DIR.
 *
 * The real library cannot be committed at the locked commit in a test, and
 * the lanes have no libusb headers, so the tree is a stub with the same file
 * names and the same API subset the adapter uses, and the lock is the test's
 * own (PINEDIO_LOCK_FILE), and -lusb-1.0 resolves to an empty stub library
 * via LDFLAGS. The real pinned library is built by CI's normal
 * lane (.github/workflows/ci.yml).
 *
 * The daemon compile itself is a stub (CXX = a script that records its
 * arguments and writes the output file, --debug so no hardening check runs on
 * it): this test is about build.sh's handling of the pine64 tree, and the
 * real daemon compile is run_tests.sh's own build. The stub's record shows
 * the daemon gets the lock's commit as LORAHAM_CH341_PIN_EXPECTED and -ldl.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <limits.h>
#include <sys/stat.h>
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

static std::string g_tmp;
static std::string g_build_sh;

static int sh(const std::string &cmd)
{
    int rc = system(cmd.c_str());
    if (rc == -1 || !WIFEXITED(rc))
        return -1;
    return WEXITSTATUS(rc);
}

static bool file_exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool log_contains(const std::string &log, const char *needle)
{
    std::string cmd = "grep -qF -- '" + std::string(needle) + "' '" + log + "'";
    return sh(cmd) == 0;
}

/* A stub of the library: the two file names build.sh looks for and the API
 * subset ch341_adapter.c calls, plus a copy of tests/fakes/stub_libusb's
 * libusb header (the lanes have no libusb; build.sh puts the tree on the
 * include path, so the adapter and the monitor compile against it). */
static void write_stub_tree(const std::string &dir)
{
    sh("mkdir -p '" + dir + "/libusb-1.0'");
    std::string fakes = g_build_sh.substr(0, g_build_sh.rfind('/')) + "/tests/fakes/stub_libusb";
    sh("cp '" + fakes + "/libusb-1.0/libusb.h' '" + dir + "/libusb-1.0/libusb.h'");
    FILE *h = fopen((dir + "/libpinedio-usb.h").c_str(), "w");
    fputs("#include <stdint.h>\n#include <stdbool.h>\n#include <libusb-1.0/libusb.h>\n"
          "enum pinedio_int_pin { PINEDIO_PIN_D0, PINEDIO_INT_PIN_MAX = 11 };\n"
          "enum pinedio_int_mode { PINEDIO_INT_MODE_RISING = 1, PINEDIO_INT_MODE_FALLING = 2 };\n"
          "enum pinedio_option { PINEDIO_OPTION_AUTO_CS, PINEDIO_OPTION_SEARCH_SERIAL,"
          " PINEDIO_OPTION_VID, PINEDIO_OPTION_PID, PINEDIO_OPTION_MAX };\n"
          "struct pinedio_inst { libusb_device_handle *handle; void *transfer_out; bool in_error;"
          " uint32_t options[PINEDIO_OPTION_MAX]; char serial_number[9]; };\n"
          "int32_t pinedio_init(struct pinedio_inst *i, void *d);\n"
          "int32_t pinedio_set_option(struct pinedio_inst *i, enum pinedio_option o, uint32_t v);\n"
          "int32_t pinedio_set_pin_mode(struct pinedio_inst *i, uint32_t p, uint32_t m);\n"
          "int32_t pinedio_digital_write(struct pinedio_inst *i, uint32_t p, bool a);\n"
          "int32_t pinedio_digital_read(struct pinedio_inst *i, uint32_t p);\n"
          "int32_t pinedio_transceive(struct pinedio_inst *i, uint8_t *w, uint8_t *r, uint32_t n);\n"
          "int32_t pinedio_attach_interrupt(struct pinedio_inst *i, enum pinedio_int_pin p,"
          " enum pinedio_int_mode m, void (*cb)(void));\n"
          "int32_t pinedio_deattach_interrupt(struct pinedio_inst *i, enum pinedio_int_pin p);\n"
          "void pinedio_deinit(struct pinedio_inst *i);\n", h);
    fclose(h);
    FILE *c = fopen((dir + "/libpinedio-usb.c").c_str(), "w");
    fputs("#include \"libpinedio-usb.h\"\n"
          "uint16_t pinedio_d_mode, pinedio_d_state;\n"
          "int32_t pinedio_init(struct pinedio_inst *i, void *d) { (void)i; (void)d; return -2; }\n"
          "int32_t pinedio_set_option(struct pinedio_inst *i, enum pinedio_option o, uint32_t v)"
          " { i->options[o] = v; return 0; }\n"
          "int32_t pinedio_set_pin_mode(struct pinedio_inst *i, uint32_t p, uint32_t m)"
          " { (void)i; (void)p; (void)m; return 0; }\n"
          "int32_t pinedio_digital_write(struct pinedio_inst *i, uint32_t p, bool a)"
          " { (void)i; (void)p; (void)a; return 0; }\n"
          "int32_t pinedio_digital_read(struct pinedio_inst *i, uint32_t p) { (void)i; (void)p; return 0; }\n"
          "int32_t pinedio_transceive(struct pinedio_inst *i, uint8_t *w, uint8_t *r, uint32_t n)"
          " { (void)i; (void)w; (void)r; (void)n; return 0; }\n"
          "int32_t pinedio_attach_interrupt(struct pinedio_inst *i, enum pinedio_int_pin p,"
          " enum pinedio_int_mode m, void (*cb)(void)) { (void)i; (void)p; (void)m; (void)cb; return 0; }\n"
          "int32_t pinedio_deattach_interrupt(struct pinedio_inst *i, enum pinedio_int_pin p)"
          " { (void)i; (void)p; return 0; }\n"
          "void pinedio_deinit(struct pinedio_inst *i) { (void)i; }\n", c);
    fclose(c);
}

static const char *GIT = "git -c user.name=t -c user.email=t@t -c init.defaultBranch=main";

static std::string git_head(const std::string &dir)
{
    std::string out = g_tmp + "/head.txt";
    sh("git -C '" + dir + "' rev-parse HEAD > '" + out + "'");
    char buf[64] = {0};
    FILE *f = fopen(out.c_str(), "r");
    if (f) {
        if (!fgets(buf, sizeof(buf), f))
            buf[0] = '\0';
        fclose(f);
    }
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

static void write_lock(const std::string &path, const std::string &commit)
{
    FILE *f = fopen(path.c_str(), "w");
    fprintf(f, "PINEDIO_URL=https://example.invalid/stub.git\nPINEDIO_COMMIT=%s\n", commit.c_str());
    fclose(f);
}

/* Run build.sh with this pine64 source (empty = PINEDIO_DIR unset) and lock,
 * into its own output directory; the log is kept for the checks. */
static int run_build(const char *name, const std::string &pinedio_dir,
                     const std::string &lock, std::string *out_dir, std::string *log)
{
    *out_dir = g_tmp + "/out-" + name;
    *log = g_tmp + "/log-" + name + ".txt";
    sh("mkdir -p '" + *out_dir + "' '" + g_tmp + "/home'");
    std::string env = "HOME='" + g_tmp + "/home' PINEDIO_LOCK_FILE='" + lock + "' CXX='" +
                      g_tmp + "/fake-cxx' FAKE_CXX_RECORD='" + *out_dir + "/cxx-args.txt' LDFLAGS='-L" +
           g_tmp + "/fakeusb' ";
    env += pinedio_dir.empty() ? "env -u PINEDIO_DIR " : "PINEDIO_DIR='" + pinedio_dir + "' ";
    return sh(env + "'" + g_build_sh + "' --debug --output '" + *out_dir + "/loraham_daemon' > '" +
              *log + "' 2>&1");
}

static void expect_refused(const char *name, const std::string &dir, const std::string &lock,
                           const char *reason)
{
    std::string out, log;
    int rc = run_build(name, dir, lock, &out, &log);
    std::string label = std::string(name) + ": build refused";
    expect(label.c_str(), rc != 0);
    label = std::string(name) + ": reason \"" + reason + "\"";
    expect(label.c_str(), log_contains(log, reason));
    label = std::string(name) + ": no library, no daemon";
    expect(label.c_str(), !file_exists(out + "/lib/libloraham-ch341.so") &&
                          !file_exists(out + "/loraham_daemon"));
}

int main(void)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        printf("[FAIL] cannot resolve /proc/self/exe\n");
        return 1;
    }
    exe[n] = '\0';
    std::string tests_dir(exe, strrchr(exe, '/') - exe);
    g_build_sh = tests_dir + "/../build.sh";

    char tmpl[] = "/tmp/loraham-build-ch341.XXXXXX";
    if (!mkdtemp(tmpl)) {
        printf("[FAIL] mkdtemp\n");
        return 1;
    }
    g_tmp = tmpl;

    /* The stub daemon compiler: records its arguments, writes -o. */
    {
        FILE *f = fopen((g_tmp + "/fake-cxx").c_str(), "w");
        fputs("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$FAKE_CXX_RECORD\"\n"
              "while [ $# -gt 0 ]; do if [ \"$1\" = -o ]; then : > \"$2\"; fi; shift; done\n", f);
        fclose(f);
        sh("chmod 755 '" + g_tmp + "/fake-cxx'");
    }

    /* An empty libusb-1.0 to link the stub library against. */
    sh("mkdir -p '" + g_tmp + "/fakeusb' && echo 'void loraham_test_fakeusb(void) {}' > '" + g_tmp +
       "/fakeusb/u.c' && cc -shared -fPIC -o '" + g_tmp + "/fakeusb/libusb-1.0.so' '" + g_tmp +
       "/fakeusb/u.c'");

    /* The locked tree: its own repository, one commit. */
    std::string pine = g_tmp + "/pine";
    write_stub_tree(pine);
    sh(std::string(GIT) + " -C '" + pine + "' init -q && " + GIT + " -C '" + pine +
       "' add -A && " + GIT + " -C '" + pine + "' commit -q -m stub");
    std::string head = git_head(pine);
    std::string lock = g_tmp + "/libpinedio.lock";
    write_lock(lock, head);
    expect("stub tree committed", head.size() == 40);

    /* Refusals (each stops before the daemon is compiled). */
    std::string wrong_lock = g_tmp + "/wrong.lock";
    write_lock(wrong_lock, std::string(40, '0'));
    expect_refused("wrong-commit", pine, wrong_lock, "the lock requires");

    sh("echo '/* local edit */' >> '" + pine + "/libpinedio-usb.c'");
    expect_refused("modified", pine, lock, "has local changes or extra files");
    sh("git -C '" + pine + "' checkout -q -- libpinedio-usb.c");

    sh("touch '" + pine + "/extra.c'");
    expect_refused("extra-file", pine, lock, "has local changes or extra files");
    sh("rm -f '" + pine + "/extra.c'");

    std::string plain = g_tmp + "/plain";
    write_stub_tree(plain);
    expect_refused("not-git", plain, lock, "is not a git checkout");

    std::string outer = g_tmp + "/outer";
    write_stub_tree(outer + "/sub");
    sh(std::string(GIT) + " -C '" + outer + "' init -q && " + GIT + " -C '" + outer +
       "' add -A && " + GIT + " -C '" + outer + "' commit -q -m outer");
    write_lock(g_tmp + "/outer.lock", git_head(outer));
    expect_refused("inside-other-repo", outer + "/sub", g_tmp + "/outer.lock",
                   "is not the top of its own checkout");

    sh("mkdir -p '" + g_tmp + "/empty'");
    expect_refused("unusable-dir", g_tmp + "/empty", lock, "PINEDIO_DIR is set but not usable");

    /* The locked, clean tree: library with the measured commit, then the daemon. */
    {
        std::string out, log;
        int rc = run_build("locked", pine, lock, &out, &log);
        std::string so = out + "/lib/libloraham-ch341.so";
        expect("locked: build succeeds", rc == 0);
        if (rc != 0)
            sh("cat '" + log + "'");
        expect("locked: library built", file_exists(so));
        expect("locked: daemon built", file_exists(out + "/loraham_daemon"));
        expect("locked: library embeds the measured commit",
               sh("grep -qaF '" + head + "' '" + so + "'") == 0);
        expect("locked: library exports loraham_ch341_pin",
               sh("nm -D --defined-only '" + so + "' | grep -qw loraham_ch341_pin") == 0);
        expect("locked: build log names the commit", log_contains(log, head.c_str()));
        expect("locked: daemon gets the lock's commit as LORAHAM_CH341_PIN_EXPECTED",
               log_contains(out + "/cxx-args.txt",
                            ("-DLORAHAM_CH341_PIN_EXPECTED=\"" + head + "\"").c_str()));
        expect("locked: daemon links -ldl", sh("grep -qx -- -ldl '" + out + "/cxx-args.txt'") == 0);
    }

    /* The same output directory rebuilt without a tree: the library of the
     * earlier build must not stay in the bundle. */
    {
        std::string out, log;
        int rc = run_build("reuse", pine, lock, &out, &log);
        expect("reuse: first build (locked tree) makes the library",
               rc == 0 && file_exists(out + "/lib/libloraham-ch341.so"));
        rc = run_build("reuse", "", lock, &out, &log);
        expect("reuse: rebuild without a tree succeeds", rc == 0);
        expect("reuse: the earlier library is gone", !file_exists(out + "/lib/libloraham-ch341.so"));
        expect("reuse: the daemon is built", file_exists(out + "/loraham_daemon"));
        expect("reuse: says so", log_contains(log, "Meshstick library not built (no pine64 source)"));
        expect("reuse: names the removed file", log_contains(log, "Removed the earlier Meshstick library"));
    }

    /* No tree anywhere: the daemon alone, as before this feature. */
    {
        std::string out, log;
        int rc = run_build("none", "", lock, &out, &log);
        expect("no tree: build succeeds", rc == 0);
        expect("no tree: says so", log_contains(log, "Meshstick library not built (no pine64 source)"));
        expect("no tree: no library", !file_exists(out + "/lib/libloraham-ch341.so"));
        expect("no tree: daemon built", file_exists(out + "/loraham_daemon"));
        expect("no tree: daemon still gets the lock's commit",
               log_contains(out + "/cxx-args.txt",
                            ("-DLORAHAM_CH341_PIN_EXPECTED=\"" + head + "\"").c_str()));
    }

    sh("rm -rf '" + g_tmp + "'");
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
