/*
 * The Meshstick library loader refuses every library it cannot fully trust.
 *
 * daemon_ch341_load_from() must accept exactly one thing: a loadable .so that
 * exports every adapter symbol and reports the pine64 commit the daemon
 * expects. The test builds fake adapters with the system C compiler at run
 * time (the real library needs libusb and a CH341, neither of which a lane
 * has) and checks each refusal reason: missing file, a file that is not a
 * shared object, a library whose own dependency is missing (as a host without
 * libusb-1.0 would be), a missing symbol, the wrong commit, an empty expected
 * commit. On every refusal the caller's table stays untouched.
 */

#include "../ch341_loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dlfcn.h>
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

static const char *PIN_GOOD = "b0694ec84091b293cb1e97e00a821ff88353274f";
static const char *PIN_OTHER = "0000000000000000000000000000000000000000";

/* The adapter's ten functions; `omit` leaves one out, `pin` is what it reports. */
static void write_fake_source(const std::string &path, const char *pin, const char *omit)
{
    static const char *funcs[] = {
        "loraham_ch341 *loraham_ch341_open(uint16_t v, uint16_t p, const char *s)"
        " { (void)v; (void)p; (void)s; return 0; }",
        "void loraham_ch341_close(loraham_ch341 *d) { (void)d; }",
        "int loraham_ch341_transceive(loraham_ch341 *d, const uint8_t *o, uint8_t *i, uint32_t n)"
        " { (void)d; (void)o; (void)i; (void)n; return 0; }",
        "int loraham_ch341_pin_mode(loraham_ch341 *d, uint32_t p, int o) { (void)d; (void)p; (void)o; return 0; }",
        "int loraham_ch341_digital_write(loraham_ch341 *d, uint32_t p, int l) { (void)d; (void)p; (void)l; return 0; }",
        "int loraham_ch341_digital_read(loraham_ch341 *d, uint32_t p) { (void)d; (void)p; return 0; }",
        "int loraham_ch341_attach_interrupt(loraham_ch341 *d, uint32_t p, uint32_t m, void (*c)(void))"
        " { (void)d; (void)p; (void)m; (void)c; return 0; }",
        "int loraham_ch341_detach_interrupt(loraham_ch341 *d, uint32_t p) { (void)d; (void)p; return 0; }",
        "int loraham_ch341_in_error(loraham_ch341 *d) { (void)d; return 0; }",
    };
    FILE *f = fopen(path.c_str(), "w");
    fprintf(f, "#include <stdint.h>\ntypedef struct loraham_ch341 loraham_ch341;\n");
    fprintf(f, "const char *loraham_ch341_pin(void) { return \"%s\"; }\n", pin);
    for (const char *fn : funcs)
        if (!omit || !strstr(fn, omit))
            fprintf(f, "%s\n", fn);
    fclose(f);
}

static bool build_so(const std::string &name, const char *pin, const char *omit,
                     const std::string &extra = "")
{
    std::string src = g_tmp + "/" + name + ".c";
    write_fake_source(src, pin, omit);
    std::string cmd = "cc -shared -fPIC -o '" + g_tmp + "/" + name + ".so' '" + src + "' " + extra;
    return system(cmd.c_str()) == 0;
}

static bool load(const std::string &name, const char *expected, LorahamCh341Api *api,
                 std::string *why_out)
{
    char why[512] = "";
    std::string path = g_tmp + "/" + name + ".so";
    bool ok = daemon_ch341_load_from(path.c_str(), expected, api, why, sizeof(why));
    *why_out = why;
    printf("       %s -> %s\n", name.c_str(), ok ? "loaded" : why);
    return ok;
}

static bool untouched(const LorahamCh341Api &api)
{
    static const LorahamCh341Api zero = {};
    return memcmp(&api, &zero, sizeof(api)) == 0;
}

static void refused(const char *label, const std::string &name, const char *expected,
                    const char *reason)
{
    LorahamCh341Api api = {};
    std::string why;
    bool ok = load(name, expected, &api, &why);
    std::string t = std::string(label) + ": refused";
    expect(t.c_str(), !ok);
    t = std::string(label) + ": reason names \"" + reason + "\"";
    expect(t.c_str(), why.find(reason) != std::string::npos);
    t = std::string(label) + ": table untouched";
    expect(t.c_str(), untouched(api));
    /* A refused library that did load is unloaded again (RTLD_NOLOAD finds
     * only a library that is still loaded). */
    std::string path = g_tmp + "/" + name + ".so";
    void *still = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    t = std::string(label) + ": not left loaded";
    expect(t.c_str(), still == nullptr);
    if (still)
        dlclose(still);
}

int main(void)
{
    char tmpl[] = "/tmp/loraham-ch341-loader.XXXXXX";
    if (!mkdtemp(tmpl)) {
        printf("[FAIL] mkdtemp\n");
        return 1;
    }
    g_tmp = tmpl;

    expect("fake: good", build_so("good", PIN_GOOD, nullptr));
    expect("fake: wrong commit", build_so("wrongpin", PIN_OTHER, nullptr));
    expect("fake: missing in_error", build_so("nosym", PIN_GOOD, "loraham_ch341_in_error"));

    /* A library that needs another shared object which is then gone: what a
     * host without libusb-1.0 does to the real one. */
    {
        std::string dep = g_tmp + "/dep.c";
        FILE *f = fopen(dep.c_str(), "w");
        fputs("void loraham_test_dep(void) {}\n", f);
        fclose(f);
        bool built = system(("cc -shared -fPIC -o '" + g_tmp + "/libloraham-test-dep.so' '" + dep + "'").c_str()) == 0 &&
                     build_so("needsdep", PIN_GOOD, nullptr,
                              "-L'" + g_tmp + "' -Wl,--no-as-needed -lloraham-test-dep");
        expect("fake: with a dependency", built);
        unlink((g_tmp + "/libloraham-test-dep.so").c_str());
    }

    /* Not a shared object at all. */
    {
        FILE *f = fopen((g_tmp + "/notelf.so").c_str(), "w");
        fputs("this is not a shared object\n", f);
        fclose(f);
    }

    refused("absent", "absent", PIN_GOOD, "Meshstick library not found");
    refused("not ELF", "notelf", PIN_GOOD, "cannot load");
    refused("dependency missing", "needsdep", PIN_GOOD, "libloraham-test-dep.so");
    refused("missing symbol", "nosym", PIN_GOOD, "missing symbol loraham_ch341_in_error");
    refused("wrong commit", "wrongpin", PIN_GOOD, "this daemon expects b0694ec8");
    refused("no expected commit", "good", "", "no lock at build time");

    {
        LorahamCh341Api api = {};
        std::string why;
        bool ok = load("good", PIN_GOOD, &api, &why);
        expect("good: loaded", ok);
        expect("good: every entry resolved",
               api.pin && api.open && api.close && api.transceive && api.pin_mode &&
               api.digital_write && api.digital_read && api.attach_interrupt &&
               api.detach_interrupt && api.in_error);
        expect("good: reports the expected commit", ok && strcmp(api.pin(), PIN_GOOD) == 0);
    }

    /* The bundle path is the binary's own directory + lib/. */
    {
        char path[4096];
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        exe[n > 0 ? n : 0] = '\0';
        std::string want = std::string(exe, strrchr(exe, '/') - exe) + "/lib/libloraham-ch341.so";
        expect("bundle path = <exe dir>/lib/libloraham-ch341.so",
               daemon_ch341_library_path(path, sizeof(path)) && want == path);
        expect("bundle path: too small a buffer is refused",
               !daemon_ch341_library_path(path, 8));
    }

    int rc = system(("rm -rf '" + g_tmp + "'").c_str());
    (void)rc;
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
