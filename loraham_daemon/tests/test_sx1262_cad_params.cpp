/*
 * SX126x CAD parameters through the public paths, on the REAL Sx1262Driver and
 * the REAL pinned RadioLib, with tests/fakes/sx126x_register_model.h where the
 * chip would be.
 *
 * A CONF client sends SET CADSYM / CADPEAK / CADMIN through the dispatch line
 * entry; GET CHANNEL (radio_cad_try_probe) and the MANAGED gate's probe
 * (radio_cad_probe, daemon_data_tx_runtime.cpp) then scan. The model keeps the
 * bytes of the last SetCadParams command, so each test reads what the chip
 * would have been told. Before any SET, those bytes must equal what RadioLib's
 * own no-argument scanChannel() sends where the per-SF table leaves every field
 * to RadioLib (SF7, any bandwidth other than 125 kHz), and the table entry at
 * SF8..SF12 / BW 125 kHz.
 *
 * Uses only interfaces that predate the setters, so the same file runs against
 * the earlier sources (red-before).
 */

#include "../config_dispatch.h"
#include "../config_status.h"
#include "../radio_cad.h"
#include "../sx1262_driver.h"
#include "fakes/sx126x_register_model.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

static int g_ok = 0;
static int g_fail = 0;

static void expect_true(const char *name, bool cond, const std::string &detail)
{
    if (cond) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: %s\n", name, detail.c_str());
    }
}

static std::string hex7(const uint8_t *b)
{
    char s[64];

    snprintf(s, sizeof(s), "%02X %02X %02X %02X %02X %02X %02X",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
    return s;
}

/* The SetDioIrqParams RadioLib's own no-argument scan sends before its
 * SetCadParams (captured once by test_no_set_is_radiolib_default_bytes). */
static uint8_t g_ref_dio_irq[8];
static bool g_have_ref_dio_irq = false;

static std::string hex8(const uint8_t *b)
{
    std::string out;
    for (int i = 0; i < 8; i++) {
        char h[4];

        snprintf(h, sizeof(h), i ? " %02X" : "%02X", b[i]);
        out += h;
    }
    return out;
}

/* The bytes of the last SetCadParams, against [symCode, detPeak, detMin]
 * + exit mode STDBY + timeout 0 (what the no-argument scan always sends), and
 * the SetDioIrqParams in force for it against RadioLib's own scan. */
static void expect_cad(const char *name, const Sx126xRegisterModel &m,
                       uint8_t sym_code, uint8_t peak, uint8_t min)
{
    const uint8_t want[7] = { sym_code, peak, min, 0x00, 0x00, 0x00, 0x00 };

    expect_true(name, memcmp(m.cad_params, want, 7) == 0,
                "SetCadParams " + hex7(m.cad_params) + ", want " + hex7(want));
    expect_true((std::string(name) + " (IRQ params == RadioLib's scan)").c_str(),
                g_have_ref_dio_irq && memcmp(m.cad_dio_irq, g_ref_dio_irq, 8) == 0,
                "SetDioIrqParams " + hex8(m.cad_dio_irq) + ", want " + hex8(g_ref_dio_irq));
}

static void expect_has(const char *name, const std::string &s, const char *needle)
{
    expect_true(name, s.find(needle) != std::string::npos,
                std::string("no '") + needle + "' in '" + s + "'");
}

/* --- Async TX runtime status stubs (config_status reads them) ------------ */

size_t daemon_tx_async_runtime_pending() { return 0; }
int daemon_tx_async_runtime_job_active() { return 0; }
size_t daemon_tx_async_runtime_dropped() { return 0; }
size_t daemon_tx_async_runtime_rejected() { return 0; }
size_t daemon_tx_async_runtime_processed() { return 0; }
size_t daemon_tx_async_runtime_completion_stale() { return 0; }
size_t daemon_tx_async_runtime_completion_dropped() { return 0; }
int daemon_tx_async_runtime_last_result(DaemonTxJobResult *) { return 0; }

static void rx_callback(void) { }

/* The apply path is not under test: SET with RF keys never reaches it here. */
static ConfigApplyStatus no_apply(RadioDriver &, const char *, const char *,
                                  RadioMode_t &, std::atomic<bool> &)
{
    return CONFIG_APPLY_UNKNOWN;
}

/* --- rig ----------------------------------------------------------------- */

static const int PIN_CS = 21, PIN_IRQ = 16, PIN_RST = 18, PIN_BUSY = 20;

static RadioRfDefaults lora_defaults(int sf, float bw_khz)
{
    RadioRfDefaults def{};

    def.freq_mhz = 869.525f;
    def.spreading_factor = sf;
    def.bandwidth_khz = bw_khz;
    def.sync_word = 0x12;
    def.preamble_len = 8;
    def.coding_rate = 5;
    def.crc_on = true;
    def.ldro = -1;
    def.power_dbm = 10;
    return def;
}

struct Rig {
    Sx126xRegisterModel model;
    Module mod;
    RadioController ctrl;
    RadioRfDefaults def;
    int sv[2] = { -1, -1 };
    ClientSlot slot;

    explicit Rig(int sf = 12, float bw_khz = 125.0f)
      : mod(&model, PIN_CS, PIN_IRQ, PIN_RST, PIN_BUSY), def(lora_defaults(sf, bw_khz))
    {
        model.reset();
        model.irq_high_pin = PIN_IRQ;                     /* CAD ends at once */
        model.irq_status = RADIOLIB_SX126X_IRQ_CAD_DONE;  /* channel free */
        radio_controller_init(&ctrl, RADIO_BAND_868, "T", true, rx_callback, -1);
        ctrl.chip_family = DAEMON_CHIP_FAMILY_SX1262;
        new_driver();
        ctrl.health = RADIO_HEALTH_READY;

        client_slot_init_all(&slot, 1);
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0)
            client_slot_set_fd(&slot, sv[1]);
    }

    ~Rig()
    {
        client_slot_close(&slot);
        if (sv[0] >= 0)
            close(sv[0]);
    }

    /* A new driver object and boot, as the USB re-init does (radio_controller.h:167). */
    void new_driver()
    {
        ctrl.driver.reset(sx1262_driver_create(&mod, 1.8f, -1, 100, false));
        ctrl.driver->begin(&def);
    }

    /* One CONF line through the dispatch line entry; the reply line. */
    std::string ask(const char *line)
    {
        ConfigLineApplyContext lctx = { &slot, &ctrl, "T", no_apply, { NULL, NULL, NULL } };
        char buf[1200];

        config_dispatch_apply_line(line, &lctx);
        ssize_t n = recv(sv[0], buf, sizeof(buf) - 1, MSG_DONTWAIT);
        buf[n > 0 ? n : 0] = '\0';
        return buf;
    }

    /* GET CHANNEL must have scanned (CADSCAN=1) for the bytes to be this scan's. */
    void get_channel(const char *name)
    {
        model.cad_params_count = 0;
        std::string r = ask("GET CHANNEL");
        expect_true(name, model.cad_params_count == 1 && r.find(" CADSCAN=1 ") != std::string::npos,
                    "scans " + std::to_string(model.cad_params_count) + ", reply '" + r + "'");
    }

    void managed_probe(const char *name)
    {
        model.cad_params_count = 0;
        RadioCadProbeResult p = radio_cad_probe(&ctrl);
        expect_true(name, model.cad_params_count == 1 && p.scan_ran == 1,
                    "scans " + std::to_string(model.cad_params_count));
    }
};

/* --- tests ----------------------------------------------------------------- */

/* Before any SET, where the table leaves every field to RadioLib (SF7 at
 * BW 125 kHz): both probe sites send RadioLib's own no-argument scan bytes. */
static void test_no_set_is_radiolib_default_bytes(void)
{
    uint8_t want[7];

    {
        /* RadioLib alone, same boot values, no daemon code. */
        Sx126xRegisterModel ref_model;
        Module ref_mod(&ref_model, PIN_CS, PIN_IRQ, PIN_RST, PIN_BUSY);
        SX1262 ref(&ref_mod);

        ref_model.irq_high_pin = PIN_IRQ;
        ref_model.irq_status = RADIOLIB_SX126X_IRQ_CAD_DONE;
        ref.begin(869.525f, 125.0f, 7, 5, 0x12, 10, 8, 1.8f, false);
        expect_true("RadioLib no-arg scan: free",
                    ref.scanChannel() == RADIOLIB_CHANNEL_FREE, "not CHANNEL_FREE");
        memcpy(want, ref_model.cad_params, 7);
        memcpy(g_ref_dio_irq, ref_model.cad_dio_irq, 8);
        g_have_ref_dio_irq = true;
        expect_true("RadioLib no-arg scan: CadDone/CadDetected on DIO1",
                    (g_ref_dio_irq[3] & 0x80) && (g_ref_dio_irq[2] & 0x01), hex8(g_ref_dio_irq));
        expect_true("RadioLib no-arg scan at SF7: 4 symbols, detPeak 20, detMin 10",
                    want[0] == RADIOLIB_SX126X_CAD_ON_4_SYMB && want[1] == 20 && want[2] == 10,
                    hex7(want));
    }

    Rig rig(7);

    rig.get_channel("no SET: GET CHANNEL scanned");
    expect_true("no SET: GET CHANNEL bytes == RadioLib no-arg",
                memcmp(rig.model.cad_params, want, 7) == 0,
                hex7(rig.model.cad_params) + " vs " + hex7(want));
    expect_true("no SET: GET CHANNEL IRQ params == RadioLib no-arg",
                memcmp(rig.model.cad_dio_irq, g_ref_dio_irq, 8) == 0,
                hex8(rig.model.cad_dio_irq) + " vs " + hex8(g_ref_dio_irq));
    rig.managed_probe("no SET: MANAGED probe scanned");
    expect_true("no SET: MANAGED probe bytes == RadioLib no-arg",
                memcmp(rig.model.cad_params, want, 7) == 0,
                hex7(rig.model.cad_params) + " vs " + hex7(want));
    expect_true("no SET: MANAGED probe IRQ params == RadioLib no-arg",
                memcmp(rig.model.cad_dio_irq, g_ref_dio_irq, 8) == 0,
                hex8(rig.model.cad_dio_irq) + " vs " + hex8(g_ref_dio_irq));
}

/* Before any SET at BW 125 kHz, SF8..SF12: both probe sites send the measured
 * table entry (4 symbols, detPeak per SF, detMin 10), and STATUS shows it. */
static void test_auto_table_at_bw125(void)
{
    static const struct { int sf; uint8_t peak; const char *status; } k[] = {
        { 8, 23, " CADSYM=4 CADPEAK=23 CADMIN=10\n" },
        { 9, 27, " CADSYM=4 CADPEAK=27 CADMIN=10\n" },
        { 10, 27, " CADSYM=4 CADPEAK=27 CADMIN=10\n" },
        { 11, 28, " CADSYM=4 CADPEAK=28 CADMIN=10\n" },
        { 12, 31, " CADSYM=4 CADPEAK=31 CADMIN=10\n" },
    };

    for (const auto &e : k) {
        Rig rig(e.sf);
        std::string n = "SF" + std::to_string(e.sf) + " AUTO";

        rig.get_channel((n + ": GET CHANNEL scanned").c_str());
        expect_cad((n + ": GET CHANNEL sends 4, " + std::to_string(e.peak) + ", 10").c_str(),
                   rig.model, RADIOLIB_SX126X_CAD_ON_4_SYMB, e.peak, 10);
        rig.managed_probe((n + ": MANAGED probe scanned").c_str());
        expect_cad((n + ": MANAGED sends 4, " + std::to_string(e.peak) + ", 10").c_str(),
                   rig.model, RADIOLIB_SX126X_CAD_ON_4_SYMB, e.peak, 10);
        expect_has((n + ": STATUS").c_str(), rig.ask("GET STATUS"), e.status);
    }
}

/* SET CADPEAK reaches the chip through GET CHANNEL. */
static void test_set_cadpeak_reaches_get_channel(void)
{
    Rig rig;

    std::string r = rig.ask("SET CADPEAK=28");

    expect_true("SET CADPEAK=28 answers OK", r == "OK\n", "reply '" + r + "'");
    rig.get_channel("CADPEAK=28: GET CHANNEL scanned");
    expect_cad("CADPEAK=28: GET CHANNEL sends detPeak 28", rig.model, RADIOLIB_SX126X_CAD_ON_4_SYMB, 28, 10);
}

/* All three, through the MANAGED gate's probe, lower case, and back to AUTO. */
static void test_set_all_reach_managed_probe(void)
{
    Rig rig;

    expect_true("set cadsym=8 OK", rig.ask("set cadsym=8") == "OK\n", "not OK");
    expect_true("SET CADPEAK=30 OK", rig.ask("SET CADPEAK=30") == "OK\n", "not OK");
    expect_true("SET CADMIN=12 OK", rig.ask("SET CADMIN=12") == "OK\n", "not OK");
    rig.managed_probe("explicit: MANAGED probe scanned");
    expect_cad("explicit: MANAGED sends 8 symbols (code 0x03), 30, 12", rig.model,
               RADIOLIB_SX126X_CAD_ON_8_SYMB, 30, 12);
    expect_has("explicit: STATUS shows them", rig.ask("GET STATUS"),
               " CADSYM=8 CADPEAK=30 CADMIN=12\n");

    expect_true("SET CADSYM=AUTO OK", rig.ask("SET CADSYM=AUTO") == "OK\n", "not OK");
    expect_true("SET CADPEAK=auto OK", rig.ask("SET CADPEAK=auto") == "OK\n", "not OK");
    expect_true("SET CADMIN=AUTO OK", rig.ask("SET CADMIN=AUTO") == "OK\n", "not OK");
    rig.managed_probe("AUTO again: MANAGED probe scanned");
    expect_cad("AUTO again: the SF12 table entry (4, 31, 10)", rig.model,
               RADIOLIB_SX126X_CAD_ON_4_SYMB, 31, 10);
}

/* Each symbol count goes out as RadioLib's code, never as the count. */
static void test_symbol_counts_map_to_codes(void)
{
    static const struct { const char *cmd; uint8_t code; } k[] = {
        { "SET CADSYM=1", RADIOLIB_SX126X_CAD_ON_1_SYMB },
        { "SET CADSYM=2", RADIOLIB_SX126X_CAD_ON_2_SYMB },
        { "SET CADSYM=4", RADIOLIB_SX126X_CAD_ON_4_SYMB },
        { "SET CADSYM=8", RADIOLIB_SX126X_CAD_ON_8_SYMB },
        { "SET CADSYM=16", RADIOLIB_SX126X_CAD_ON_16_SYMB },
    };
    Rig rig;

    for (const auto &e : k) {
        std::string n = e.cmd;

        expect_true((n + " OK").c_str(), rig.ask(e.cmd) == "OK\n", "not OK");
        rig.get_channel((n + ": scanned").c_str());
        expect_cad((n + ": code, SF12 table detPeak 31").c_str(), rig.model, e.code, 31, 10);
    }
}

/* Out-of-range values answer ERR INVALID and change nothing; a bare key ERR MALFORMED. */
static void test_invalid_values_change_nothing(void)
{
    static const char *const bad[] = {
        "SET CADSYM=3", "SET CADSYM=0", "SET CADSYM=32", "SET CADPEAK=0", "SET CADPEAK=255",
        "SET CADMIN=255", "SET CADMIN=0", "SET CADPEAK=2x", "SET CADMIN=-1", "SET CADSYM=AUTOX",
    };
    Rig rig;

    expect_true("SET CADPEAK=27 OK", rig.ask("SET CADPEAK=27") == "OK\n", "not OK");
    for (const char *b : bad) {
        std::string r = rig.ask(b);
        expect_true((std::string(b) + " -> ERR INVALID").c_str(), r == "ERR INVALID\n", "reply '" + r + "'");
    }
    std::string r = rig.ask("SET CADMIN");
    expect_true("SET CADMIN -> ERR MALFORMED", r == "ERR MALFORMED\n", "reply '" + r + "'");
    r = rig.ask("SET CADPEAK=");
    expect_true("SET CADPEAK= -> ERR MALFORMED", r == "ERR MALFORMED\n", "reply '" + r + "'");
    rig.get_channel("after invalid: scanned");
    expect_cad("after invalid: still 4, 27, 10", rig.model, RADIOLIB_SX126X_CAD_ON_4_SYMB, 27, 10);
}

/* AUTO follows the SF the driver applied; an explicit value does not. */
static void test_auto_follows_sf(void)
{
    Rig rig;

    expect_true("SET SF=7 applied", rig.ctrl.driver->applyLoraParam("T", "SF", "7") == 0, "error");
    rig.get_channel("SF7 AUTO: scanned");
    expect_cad("SF7 AUTO: RadioLib default bytes at SF7 (4, 20, 10)", rig.model, RADIOLIB_SX126X_CAD_ON_4_SYMB, 20, 10);
    expect_has("SF7 AUTO: STATUS", rig.ask("GET STATUS"), " CADSYM=AUTO CADPEAK=AUTO CADMIN=AUTO\n");

    expect_true("SET SF=11 applied", rig.ctrl.driver->applyLoraParam("T", "SF", "11") == 0, "error");
    rig.get_channel("SF11 AUTO: scanned");
    expect_cad("SF11 AUTO: the SF11 table entry (4, 28, 10)", rig.model, RADIOLIB_SX126X_CAD_ON_4_SYMB, 28, 10);

    expect_true("SET CADPEAK=30 OK", rig.ask("SET CADPEAK=30") == "OK\n", "not OK");
    expect_true("SET SF=10 applied", rig.ctrl.driver->applyLoraParam("T", "SF", "10") == 0, "error");
    rig.managed_probe("SF10 explicit peak: scanned");
    expect_cad("SF10 explicit peak 30, AUTO rest", rig.model, RADIOLIB_SX126X_CAD_ON_4_SYMB, 30, 10);
}

/* Off BW 125 AUTO leaves every field to RadioLib; an explicit value still goes out. */
static void test_other_bw_is_radiolib_default(void)
{
    Rig rig(11, 250.0f);

    rig.get_channel("BW250 AUTO: scanned");
    expect_cad("BW250 AUTO: RadioLib default bytes (4, SF+13, 10)", rig.model,
               RADIOLIB_SX126X_CAD_ON_4_SYMB, 24, 10);
    expect_has("BW250 AUTO: STATUS AUTO", rig.ask("GET STATUS"),
               " CADSYM=AUTO CADPEAK=AUTO CADMIN=AUTO\n");
    expect_true("SET CADMIN=14 OK", rig.ask("SET CADMIN=14") == "OK\n", "not OK");
    rig.get_channel("BW250 CADMIN=14: scanned");
    expect_cad("BW250 CADMIN=14", rig.model, RADIOLIB_SX126X_CAD_ON_4_SYMB, 24, 14);

    expect_true("SET BW=125 applied", rig.ctrl.driver->applyLoraParam("T", "BW", "125") == 0, "error");
    expect_has("BW125 after SET BW: STATUS explicit CADMIN, rest the SF11 entry", rig.ask("GET STATUS"),
               " CADSYM=4 CADPEAK=28 CADMIN=14\n");
}

/* The settings live in the controller: a new driver object keeps them. */
static void test_settings_survive_reinit(void)
{
    Rig rig;

    expect_true("SET CADPEAK=29 OK", rig.ask("SET CADPEAK=29") == "OK\n", "not OK");
    rig.new_driver();
    rig.managed_probe("re-init: scanned");
    expect_cad("re-init: detPeak 29 kept", rig.model, RADIOLIB_SX126X_CAD_ON_4_SYMB, 29, 10);
}

/* STATUS: "-" in FSK and without a radio. */
static void test_status_dash_without_lora(void)
{
    Rig rig;

    expect_has("LoRa SF12: STATUS table entry", rig.ask("GET STATUS"), " CADSYM=4 CADPEAK=31 CADMIN=10\n");
    expect_true("MODE FSK", rig.ctrl.driver->switchMode(RADIO_MODE_FSK, &rig.def) == 0, "error");
    rig.ctrl.mode = RADIO_MODE_FSK;
    expect_has("FSK: STATUS -", rig.ask("GET STATUS"), " CADSYM=- CADPEAK=- CADMIN=-\n");
    expect_true("MODE LORA", rig.ctrl.driver->switchMode(RADIO_MODE_LORA, &rig.def) == 0, "error");
    rig.ctrl.mode = RADIO_MODE_LORA;
    expect_has("LoRa again: STATUS table entry", rig.ask("GET STATUS"), " CADSYM=4 CADPEAK=31 CADMIN=10\n");
    rig.ctrl.driver.reset();
    rig.ctrl.health = RADIO_HEALTH_UNPLUGGED;
    expect_has("no driver: STATUS -", rig.ask("GET STATUS"), " CADSYM=- CADPEAK=- CADMIN=-\n");
}

/* No controller behind the CONF context: refused, as on a chip without them. */
static void test_no_controller_refuses(void)
{
    Rig rig;
    ConfigLineApplyContext lctx = { &rig.slot, NULL, "T", no_apply, { NULL, NULL, NULL } };
    char buf[256];

    config_dispatch_apply_line("SET CADPEAK=28", &lctx);
    ssize_t n = recv(rig.sv[0], buf, sizeof(buf) - 1, MSG_DONTWAIT);
    buf[n > 0 ? n : 0] = '\0';
    expect_true("no controller: SET CADPEAK=28 -> ERR INVALID", strcmp(buf, "ERR INVALID\n") == 0,
                std::string("reply '") + buf + "'");
}

int main(void)
{
    test_no_set_is_radiolib_default_bytes();
    test_auto_table_at_bw125();
    test_set_cadpeak_reaches_get_channel();
    test_set_all_reach_managed_probe();
    test_symbol_counts_map_to_codes();
    test_invalid_values_change_nothing();
    test_auto_follows_sf();
    test_other_bw_is_radiolib_default();
    test_settings_survive_reinit();
    test_status_dash_without_lora();
    test_no_controller_refuses();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail == 0 ? 0 : 1;
}
