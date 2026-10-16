/*
 * ch341_adapter.c on its own: what loraham_ch341_open() does with the library.
 *
 * The adapter is built only into lib/libloraham-ch341.so and needs the pine64
 * library, which needs libusb -- neither is in the lanes. So this test
 * compiles the REAL ch341_adapter.c at run time with the system C compiler
 * against a stub libpinedio-usb (same header name, the API subset the adapter
 * uses) whose results the test controls and whose calls it counts, plus a
 * small C driver that calls the adapter and prints what happened.
 *
 * What it shows: open succeeds and marks D3/D5 as outputs with chip select
 * left to the caller; if either pin-mode call fails, open returns NULL after
 * pinedio_deinit() -- no device that would break SPI is handed out; a failed
 * pinedio_init() returns NULL without a deinit (nothing was opened); close
 * runs pinedio_deinit() once.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <limits.h>
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

static std::string g_tmp;

static void write_file(const std::string &path, const char *text)
{
    FILE *f = fopen(path.c_str(), "w");
    fputs(text, f);
    fclose(f);
}

static const char *STUB_H =
    "#include <stdint.h>\n#include <stdbool.h>\n"
    "enum pinedio_int_pin { PINEDIO_PIN_D0, PINEDIO_INT_PIN_MAX = 11 };\n"
    "enum pinedio_int_mode { PINEDIO_INT_MODE_RISING = 1, PINEDIO_INT_MODE_FALLING = 2 };\n"
    "enum pinedio_option { PINEDIO_OPTION_AUTO_CS, PINEDIO_OPTION_SEARCH_SERIAL,"
    " PINEDIO_OPTION_VID, PINEDIO_OPTION_PID, PINEDIO_OPTION_MAX };\n"
    "struct pinedio_inst { bool in_error; uint32_t options[PINEDIO_OPTION_MAX]; char serial_number[9]; };\n"
    "int32_t pinedio_init(struct pinedio_inst *i, void *d);\n"
    "int32_t pinedio_set_option(struct pinedio_inst *i, enum pinedio_option o, uint32_t v);\n"
    "int32_t pinedio_set_pin_mode(struct pinedio_inst *i, uint32_t p, uint32_t m);\n"
    "int32_t pinedio_digital_write(struct pinedio_inst *i, uint32_t p, bool a);\n"
    "int32_t pinedio_digital_read(struct pinedio_inst *i, uint32_t p);\n"
    "int32_t pinedio_transceive(struct pinedio_inst *i, uint8_t *w, uint8_t *r, uint32_t n);\n"
    "int32_t pinedio_attach_interrupt(struct pinedio_inst *i, enum pinedio_int_pin p,"
    " enum pinedio_int_mode m, void (*cb)(void));\n"
    "int32_t pinedio_deattach_interrupt(struct pinedio_inst *i, enum pinedio_int_pin p);\n"
    "void pinedio_deinit(struct pinedio_inst *i);\n";

/* The stub: STUB_INIT_FAIL makes init fail, STUB_FAIL_PIN=<n> makes the
 * pin-mode call for pin n fail. It records every pin mode and counts deinit. */
static const char *STUB_C =
    "#include \"libpinedio-usb.h\"\n#include <stdlib.h>\n"
    "int g_inits, g_deinits, g_mode[8] = {-1,-1,-1,-1,-1,-1,-1,-1};\n"
    "int32_t pinedio_init(struct pinedio_inst *i, void *d) { (void)i; (void)d; g_inits++;"
    " return getenv(\"STUB_INIT_FAIL\") ? -2 : 0; }\n"
    "int32_t pinedio_set_option(struct pinedio_inst *i, enum pinedio_option o, uint32_t v)"
    " { i->options[o] = v; return 0; }\n"
    "int32_t pinedio_set_pin_mode(struct pinedio_inst *i, uint32_t p, uint32_t m) { (void)i;"
    " const char *f = getenv(\"STUB_FAIL_PIN\"); if (f && (uint32_t)atoi(f) == p) return -1;"
    " if (p < 8) g_mode[p] = (int)m; return 0; }\n"
    "int32_t pinedio_digital_write(struct pinedio_inst *i, uint32_t p, bool a) { (void)i; (void)p; (void)a; return 0; }\n"
    "int32_t pinedio_digital_read(struct pinedio_inst *i, uint32_t p) { (void)i; (void)p; return 0; }\n"
    "int32_t pinedio_transceive(struct pinedio_inst *i, uint8_t *w, uint8_t *r, uint32_t n)"
    " { (void)i; (void)w; (void)r; (void)n; return 0; }\n"
    "int32_t pinedio_attach_interrupt(struct pinedio_inst *i, enum pinedio_int_pin p,"
    " enum pinedio_int_mode m, void (*cb)(void)) { (void)i; (void)p; (void)m; (void)cb; return 0; }\n"
    "int32_t pinedio_deattach_interrupt(struct pinedio_inst *i, enum pinedio_int_pin p) { (void)i; (void)p; return 0; }\n"
    "void pinedio_deinit(struct pinedio_inst *i) { (void)i; g_deinits++; }\n";

/* The driver: open, report, close when open succeeded, report again. */
static const char *DRIVER_C =
    "#include <stdio.h>\n#include \"ch341_adapter.h\"\n"
    "extern int g_inits, g_deinits, g_mode[8];\n"
    "struct pinedio_inst;\n"
    "int main(void) {\n"
    "  loraham_ch341 *d = loraham_ch341_open(0x1A86, 0x5512, NULL);\n"
    "  printf(\"open=%s inits=%d deinits=%d mode3=%d mode5=%d\\n\", d ? \"dev\" : \"null\","
    " g_inits, g_deinits, g_mode[3], g_mode[5]);\n"
    "  if (d) { loraham_ch341_close(d); printf(\"after-close deinits=%d\\n\", g_deinits); }\n"
    "  return 0;\n}\n";

static std::string run_driver(const char *env)
{
    std::string out = g_tmp + "/out.txt";
    std::string cmd = std::string(env) + " '" + g_tmp + "/driver' > '" + out + "' 2>&1";
    int rc = system(cmd.c_str());
    (void)rc;
    std::string text;
    FILE *f = fopen(out.c_str(), "r");
    char line[256];
    while (f && fgets(line, sizeof(line), f))
        text += line;
    if (f)
        fclose(f);
    printf("       %s -> %s", env[0] ? env : "(no fault)", text.c_str());
    return text;
}

int main(void)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return 1;
    exe[n] = '\0';
    std::string src_dir = std::string(exe, strrchr(exe, '/') - exe) + "/..";

    char tmpl[] = "/tmp/loraham-ch341-adapter.XXXXXX";
    if (!mkdtemp(tmpl))
        return 1;
    g_tmp = tmpl;

    write_file(g_tmp + "/libpinedio-usb.h", STUB_H);
    write_file(g_tmp + "/stub.c", STUB_C);
    write_file(g_tmp + "/driver.c", DRIVER_C);
    std::string cc = "cc -std=gnu11 -Wall -Wextra -I'" + g_tmp + "' -I'" + src_dir +
                     "' -DLORAHAM_CH341_PIN='\"stub\"' -o '" + g_tmp + "/driver' '" + g_tmp +
                     "/driver.c' '" + g_tmp + "/stub.c' '" + src_dir + "/ch341_adapter.c'";
    expect("the real ch341_adapter.c builds against the stub", system(cc.c_str()) == 0);

    std::string ok = run_driver("");
    expect("no fault: open returns a device", ok.find("open=dev") != std::string::npos);
    expect("no fault: D3 and D5 set as outputs",
           ok.find("mode3=1 mode5=1") != std::string::npos);
    expect("no fault: nothing deinitialised while open", ok.find("open=dev inits=1 deinits=0") == 0);
    expect("no fault: close deinitialises once", ok.find("after-close deinits=1") != std::string::npos);

    std::string f3 = run_driver("STUB_FAIL_PIN=3");
    expect("D3 pin mode fails: open returns NULL", f3.find("open=null") == 0);
    expect("D3 pin mode fails: deinitialised inside open, before it returns",
           f3.find("open=null inits=1 deinits=1") == 0);

    std::string f5 = run_driver("STUB_FAIL_PIN=5");
    expect("D5 pin mode fails: open returns NULL", f5.find("open=null") == 0);
    expect("D5 pin mode fails: deinitialised inside open, before it returns",
           f5.find("open=null inits=1 deinits=1") == 0);

    std::string fi = run_driver("STUB_INIT_FAIL=1");
    expect("init fails: open returns NULL", fi.find("open=null") == 0);
    expect("init fails: nothing to deinitialise", fi.find("open=null inits=1 deinits=0") == 0);

    int rc = system(("rm -rf '" + g_tmp + "'").c_str());
    (void)rc;
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
