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
    "#include <stdint.h>\n#include <stdbool.h>\n#include <libusb-1.0/libusb.h>\n"
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
    "void pinedio_deinit(struct pinedio_inst *i);\n";

/* The stub: STUB_INIT_FAIL makes init fail, STUB_FAIL_PIN=<n> makes the
 * pin-mode call for pin n fail. It records every pin mode and counts deinit. */
static const char *STUB_C =
    "#include \"libpinedio-usb.h\"\n#include <stdlib.h>\n"
    "int g_inits, g_deinits, g_mode[8] = {-1,-1,-1,-1,-1,-1,-1,-1};\n"
    "int g_pin_writes, g_spi, g_last_mode = -1, g_last_state = -1;\n"
    "uint16_t pinedio_d_mode, pinedio_d_state;\n"
    "static libusb_device_handle *g_h = (libusb_device_handle *)&g_inits;\n"
    "int32_t pinedio_init(struct pinedio_inst *i, void *d) { (void)d; g_inits++;"
    " if (getenv(\"STUB_INIT_FAIL_EARLY\")) return -1;"      /* before libusb_init: no reference */
    " libusb_init(NULL);"
    " if (getenv(\"STUB_INIT_FAIL\")) return -2;"            /* no device: reference held */
    " if (getenv(\"STUB_ALLOC_FAIL\")) { i->handle = NULL; return 0; }"   /* deinit + 0 */
    " i->handle = g_h; i->transfer_out = getenv(\"STUB_NO_TRANSFER\") ? NULL : (void *)g_h;"
    /* pinedio_init ends with pinedio_set_cs(inst, false) (:318) =
     * pinedio_digital_write(0, false) (:371-373): one pin write of the
     * process-wide globals, state bit 0 (D0) cleared, direction as it is
     * (:350-360). */
    " pinedio_d_state &= (uint16_t)~1u; g_pin_writes++;"
    " g_last_mode = pinedio_d_mode; g_last_state = pinedio_d_state; return 0; }\n"
    "int32_t pinedio_set_option(struct pinedio_inst *i, enum pinedio_option o, uint32_t v)"
    " { i->options[o] = v; return 0; }\n"
    "int32_t pinedio_set_pin_mode(struct pinedio_inst *i, uint32_t p, uint32_t m) { (void)i;"
    " const char *f = getenv(\"STUB_FAIL_PIN\"); if (f && (uint32_t)atoi(f) == p) return -1;"
    " if (p < 8) g_mode[p] = (int)m;"
    " if (m == 1) pinedio_d_mode |= (uint16_t)(1u << p); else pinedio_d_mode &= (uint16_t)~(1u << p);"  /* :331-335 */
    " return 0; }\n"
    "int32_t pinedio_digital_write(struct pinedio_inst *i, uint32_t p, bool a) { (void)i; (void)p; (void)a; return 0; }\n"
    "int32_t pinedio_digital_read(struct pinedio_inst *i, uint32_t p) { (void)i; (void)p; return 0; }\n"
    "int32_t pinedio_transceive(struct pinedio_inst *i, uint8_t *w, uint8_t *r, uint32_t n)"
    " { (void)i; (void)w; (void)r; (void)n; g_spi++; return 0; }\n"
    "int32_t pinedio_attach_interrupt(struct pinedio_inst *i, enum pinedio_int_pin p,"
    " enum pinedio_int_mode m, void (*cb)(void)) { (void)i; (void)p; (void)m; (void)cb; return 0; }\n"
    "int32_t pinedio_deattach_interrupt(struct pinedio_inst *i, enum pinedio_int_pin p) { (void)i; (void)p; return 0; }\n"
    "void pinedio_deinit(struct pinedio_inst *i) { i->handle = NULL; g_deinits++; }\n";

/* The driver: open, report, close when open succeeded, report again.
 * DRIVER_WALK: a Rule B walk -- seed, open three sticks held at once, report
 * the pins, close all; three cycles, the references after each. */
static const char *DRIVER_C =
    "#include <stdio.h>\n#include <stdlib.h>\n#include \"ch341_adapter.h\"\n"
    "extern int g_inits, g_deinits, g_mode[8], stub_default_refs;\n"
    "extern int g_pin_writes, g_spi, g_last_mode, g_last_state;\n"
    "struct pinedio_inst;\n"
    "int main(void) {\n"
    "  if (getenv(\"DRIVER_WALK\")) {\n"
    "    for (int c = 0; c < 3; c++) {\n"
    "      loraham_ch341 *h[3]; int w0 = g_pin_writes;\n"
    "      for (int k = 0; k < 3; k++) { loraham_ch341_seed_idle(); h[k] = loraham_ch341_open(0x1A86, 0x5512, NULL); }\n"
    "      printf(\"cycle=%d held=%d writes=%d mode=%d state=%d spi=%d refs-held=%d\\n\", c,"
    " (h[0] && h[1] && h[2]) ? 3 : 0, g_pin_writes - w0, g_last_mode, g_last_state, g_spi, stub_default_refs);\n"
    "      int w1 = g_pin_writes;\n"
    "      for (int k = 0; k < 3; k++) loraham_ch341_close(h[k]);\n"
    "      printf(\"cycle=%d released writes-on-release=%d refs=%d\\n\", c, g_pin_writes - w1, stub_default_refs);\n"
    "    }\n"
    "    return 0;\n"
    "  }\n"
    "  loraham_ch341 *d = loraham_ch341_open(0x1A86, 0x5512, NULL);\n"
    "  printf(\"open=%s inits=%d deinits=%d mode3=%d mode5=%d\\n\", d ? \"dev\" : \"null\","
    " g_inits, g_deinits, g_mode[3], g_mode[5]);\n"
    "  if (d) { loraham_ch341_close(d); printf(\"after-close deinits=%d\\n\", g_deinits); }\n"
    "  printf(\"refs=%d\\n\", stub_default_refs);\n"
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
    std::string stub_dir = src_dir + "/tests/fakes/stub_libusb";
    std::string cc = "cc -std=gnu11 -Wall -Wextra -I'" + g_tmp + "' -I'" + stub_dir + "' -I'" + src_dir +
                     "' -DLORAHAM_CH341_PIN='\"stub\"' -o '" + g_tmp + "/driver' '" + g_tmp +
                     "/driver.c' '" + g_tmp + "/stub.c' '" + stub_dir + "/libusb_stub.c' '" +
                     src_dir + "/ch341_adapter.c'";
    expect("the real ch341_adapter.c builds against the stub", system(cc.c_str()) == 0);

    std::string ok = run_driver("");
    expect("no fault: open returns a device", ok.find("open=dev") != std::string::npos);
    expect("no fault: D3 and D5 set as outputs",
           ok.find("mode3=1 mode5=1") != std::string::npos);
    expect("no fault: nothing deinitialised while open", ok.find("open=dev inits=1 deinits=0") == 0);
    expect("no fault: close deinitialises once", ok.find("after-close deinits=1") != std::string::npos);
    expect("no fault: open + close leave no libusb reference", ok.find("refs=0") != std::string::npos);

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
    expect("init -2 (no device): its libusb reference released", fi.find("refs=0") != std::string::npos);

    std::string fe = run_driver("STUB_INIT_FAIL_EARLY=1");
    expect("init -1: open returns NULL", fe.find("open=null") == 0);
    expect("init -1: no reference was taken, none released", fe.find("refs=0") != std::string::npos);

    std::string fa = run_driver("STUB_ALLOC_FAIL=1");
    expect("init 0 with a NULL handle (allocation failure): open returns NULL", fa.find("open=null") == 0);
    expect("init 0 with a NULL handle: its libusb reference released", fa.find("refs=0") != std::string::npos);

    std::string ft = run_driver("STUB_NO_TRANSFER=1");
    expect("init 0 with transfer_out NULL: open returns NULL", ft.find("open=null") == 0);
    expect("init 0 with transfer_out NULL: its libusb reference released", ft.find("refs=0") != std::string::npos);

    /* A Rule B walk: RESET (D2) an output and high, D0 (NSS) low after each
     * open's one pin write; no SPI to a held stick; release writes no pin
     * (CS not restored); held references balanced over three cycles. */
    std::string fw = run_driver("DRIVER_WALK=1");
    expect("walk: three sticks held at once", fw.find("cycle=0 held=3") != std::string::npos);
    expect("walk: one pin write per attempt", fw.find("held=3 writes=3 ") != std::string::npos);
    /* mode 44 = D2 (seeded) + D3/D5 (the adapter's pin modes from the first
     * open); state 4 = D2 high, D0 low. */
    expect("walk: RESET (D2) an output and high, the D0 state bit low (mode=44 state=4)",
           fw.find("mode=44 state=4 ") != std::string::npos);
    expect("walk: no SPI transfer to a held stick", fw.find("spi=0") != std::string::npos);
    expect("walk: three references while three are held", fw.find("refs-held=3") != std::string::npos);
    expect("walk: release writes no pin (CS not restored)",
           fw.find("writes-on-release=1") == std::string::npos && fw.find("writes-on-release=0") != std::string::npos);
    expect("walk: references balanced after each of three cycles",
           fw.find("cycle=0 released writes-on-release=0 refs=0") != std::string::npos &&
           fw.find("cycle=1 released writes-on-release=0 refs=0") != std::string::npos &&
           fw.find("cycle=2 released writes-on-release=0 refs=0") != std::string::npos);

    std::string fp = run_driver("STUB_FAIL_PIN=3");
    expect("pin mode fails: the reference released after deinit", fp.find("refs=0") != std::string::npos);

    int rc = system(("rm -rf '" + g_tmp + "'").c_str());
    (void)rc;
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
