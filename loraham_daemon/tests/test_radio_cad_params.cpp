/*
 * SX126x CAD parameters: the pure resolution (radio_cad_params.h) and the SF /
 * bandwidth record the REAL Sx1262Driver keeps for it, on the REAL pinned
 * RadioLib with tests/fakes/sx126x_register_model.h where the chip would be.
 *
 * - The table holds RadioLib's "default" byte (0xFF) in every field, so AUTO is
 *   the no-argument scan by construction, at every SF and bandwidth.
 * - The table is for BW 125 kHz, matched on the applied value within 0.01 kHz;
 *   the CONF spellings 125 / 125.0 / 125.00 all land on it.
 * - The driver records SF and bandwidth only when RadioLib accepted them: a SET
 *   that RadioLib refuses, or that the driver rejects before RadioLib, leaves the
 *   record as it was.
 */

#include "../config_value.h"
#include "../radio_cad_params.h"
#include "../sx1262_driver.h"
#include "fakes/sx126x_register_model.h"

#include <math.h>
#include <stdio.h>

static int g_ok = 0;
static int g_fail = 0;

static void expect_int(const char *name, long got, long want)
{
    if (got == want) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %ld, got %ld\n", name, want, got);
    }
}

static const uint8_t D = RADIOLIB_SX126X_CAD_PARAM_DEFAULT;
static const RadioCadParamSettings ALL_AUTO = { RADIO_CAD_PARAM_AUTO, RADIO_CAD_PARAM_AUTO,
                                                RADIO_CAD_PARAM_AUTO };

static void expect_params(const char *name, RadioCadScanParams p, uint8_t sym, uint8_t peak, uint8_t min)
{
    bool ok = p.sym_code == sym && p.det_peak == peak && p.det_min == min;

    if (ok) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %02X %02X %02X, got %02X %02X %02X\n", name,
               sym, peak, min, p.sym_code, p.det_peak, p.det_min);
    }
}

/* --- resolution ------------------------------------------------------------ */

static void test_auto_is_radiolib_default_everywhere(void)
{
    static const float bws[] = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f, 0.0f };
    int bad = 0, n = 0;

    for (int sf = 5; sf <= 12; sf++) {
        for (float bw : bws) {
            RadioCadScanParams p = radio_cad_sx126x_params(sf, bw, ALL_AUTO);

            n++;
            if (p.sym_code != D || p.det_peak != D || p.det_min != D) {
                bad++;
                printf("       SF%d BW%.2f -> %02X %02X %02X\n", sf, (double)bw,
                       p.sym_code, p.det_peak, p.det_min);
            }
        }
    }
    expect_int("AUTO: every field 0xFF at SF5-12 x 11 bandwidths (88 points)", bad, 0);
    expect_int("AUTO: points checked", n, 88);
}

static void test_table_bandwidth(void)
{
    static const char *const on[] = { "125", "125.0", "125.00" };
    float bw = 0.0f;

    for (const char *s : on) {
        char name[64];

        snprintf(name, sizeof(name), "BW \"%s\" parses", s);
        expect_int(name, config_value_parse_float_exact(s, &bw), 1);
        snprintf(name, sizeof(name), "BW \"%s\" is the table bandwidth", s);
        expect_int(name, radio_cad_table_bw(bw), 1);
    }
    expect_int("BW \"250.0\" parses", config_value_parse_float_exact("250.0", &bw), 1);
    expect_int("BW 250.0 is not the table bandwidth", radio_cad_table_bw(bw), 0);
    expect_int("BW \"\" does not parse", config_value_parse_float_exact("", &bw), 0);
    expect_int("BW \"abc\" does not parse", config_value_parse_float_exact("abc", &bw), 0);
    expect_int("BW 125.009 is the table bandwidth", radio_cad_table_bw(125.009f), 1);
    expect_int("BW 125.02 is not", radio_cad_table_bw(125.02f), 0);
    expect_int("BW 124.98 is not", radio_cad_table_bw(124.98f), 0);
    expect_int("BW 0 (nothing recorded) is not", radio_cad_table_bw(0.0f), 0);
    expect_int("BW NaN is not", radio_cad_table_bw(NAN), 0);
}

static void test_symbol_map(void)
{
    static const struct { unsigned count; uint8_t code; } k[] = {
        { 1, RADIOLIB_SX126X_CAD_ON_1_SYMB }, { 2, RADIOLIB_SX126X_CAD_ON_2_SYMB },
        { 4, RADIOLIB_SX126X_CAD_ON_4_SYMB }, { 8, RADIOLIB_SX126X_CAD_ON_8_SYMB },
        { 16, RADIOLIB_SX126X_CAD_ON_16_SYMB },
    };

    for (const auto &e : k) {
        uint8_t code = 0xEE;
        char name[64];

        snprintf(name, sizeof(name), "%u symbols -> code 0x%02X", e.count, e.code);
        expect_int(name, radio_cad_sym_code(e.count, &code) && code == e.code, 1);
        snprintf(name, sizeof(name), "code 0x%02X -> %u symbols", e.code, e.count);
        expect_int(name, (long)radio_cad_sym_count(e.code), (long)e.count);
    }
    uint8_t code = 0xEE;
    expect_int("3 symbols: no code", radio_cad_sym_code(3, &code), 0);
    expect_int("0 symbols: no code", radio_cad_sym_code(0, &code), 0);
    expect_int("no code leaves the output", code, 0xEE);
    expect_int("0xFF is no symbol count", (long)radio_cad_sym_count(D), 0);
}

static void test_explicit_beats_auto(void)
{
    RadioCadParamSettings set = { 8, RADIO_CAD_PARAM_AUTO, 12 };

    expect_params("explicit CADSYM 8 + CADMIN 12, CADPEAK AUTO at SF12/BW125",
                  radio_cad_sx126x_params(12, 125.0f, set), RADIOLIB_SX126X_CAD_ON_8_SYMB, D, 12);
    set = { RADIO_CAD_PARAM_AUTO, 28, RADIO_CAD_PARAM_AUTO };
    expect_params("explicit CADPEAK 28 at BW250 (off the table)",
                  radio_cad_sx126x_params(11, 250.0f, set), D, 28, D);
    set = { 16, 254, 1 };
    expect_params("all explicit at SF7", radio_cad_sx126x_params(7, 125.0f, set),
                  RADIOLIB_SX126X_CAD_ON_16_SYMB, 254, 1);
}

/* --- the driver's SF / bandwidth record ------------------------------------ */

static RadioRfDefaults lora_defaults(void)
{
    RadioRfDefaults def{};

    def.freq_mhz = 869.525f;
    def.spreading_factor = 12;
    def.bandwidth_khz = 125.0f;
    def.sync_word = 0x12;
    def.preamble_len = 8;
    def.coding_rate = 5;
    def.crc_on = true;
    def.ldro = -1;
    def.power_dbm = 10;
    return def;
}

static void expect_record(const char *name, const Sx1262Driver &drv, int sf, float bw)
{
    int got_sf = -1;
    float got_bw = -1.0f;
    bool have = drv.loraSfBw(&got_sf, &got_bw);

    if (have && got_sf == sf && fabsf(got_bw - bw) < 0.001f) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected SF%d BW%.3f, got %s SF%d BW%.3f\n", name, sf, (double)bw,
               have ? "record" : "NO record", got_sf, (double)got_bw);
    }
}

static void test_driver_record(void)
{
    Sx126xRegisterModel model;
    Module mod(&model, 21, 16, 18, 20);
    Sx1262Driver drv(&mod, 1.8f, -1, 100);
    RadioRfDefaults def = lora_defaults();
    int sf = 0;
    float bw = 0.0f;

    expect_int("record: none before begin", drv.loraSfBw(&sf, &bw), 0);
    expect_int("record: begin OK", drv.begin(&def), RADIOLIB_ERR_NONE);
    expect_record("record: begin = band default", drv, 12, 125.0f);

    expect_int("record: SET BW=125.00 OK", drv.applyLoraParam("T", "BW", "125.00"), RADIOLIB_ERR_NONE);
    expect_record("record: BW 125.00", drv, 12, 125.0f);
    expect_int("record: SET BW=250.0 OK", drv.applyLoraParam("T", "BW", "250.0"), RADIOLIB_ERR_NONE);
    expect_record("record: BW 250", drv, 12, 250.0f);
    drv.applyLoraParam("T", "BW", "abc");
    expect_record("record: BW abc rejected before RadioLib, kept", drv, 12, 250.0f);
    drv.applyLoraParam("T", "BW", "");
    expect_record("record: BW empty rejected before RadioLib, kept", drv, 12, 250.0f);
    expect_int("record: SET SF=7 OK", drv.applyLoraParam("T", "SF", "7"), RADIOLIB_ERR_NONE);
    expect_record("record: SF 7", drv, 7, 250.0f);
    drv.applyLoraParam("T", "SF", "13");
    expect_record("record: SF 13 rejected before RadioLib, kept", drv, 7, 250.0f);

    /* RadioLib itself refuses: a LoRa setter while the chip answers GFSK. */
    model.set_packet_type(RADIOLIB_SX126X_PACKET_TYPE_GFSK);
    expect_int("record: SET SF=9 refused by RadioLib",
               drv.applyLoraParam("T", "SF", "9"), RADIOLIB_ERR_WRONG_MODEM);
    expect_int("record: SET BW=125 refused by RadioLib",
               drv.applyLoraParam("T", "BW", "125"), RADIOLIB_ERR_WRONG_MODEM);
    expect_record("record: refused SF/BW leave SF7 BW250", drv, 7, 250.0f);
    model.set_packet_type(RADIOLIB_SX126X_PACKET_TYPE_LORA);

    expect_int("record: MODE=FSK OK", drv.switchMode(RADIO_MODE_FSK, &def), RADIOLIB_ERR_NONE);
    expect_int("record: none in FSK", drv.loraSfBw(&sf, &bw), 0);
    expect_int("record: MODE=LORA OK", drv.switchMode(RADIO_MODE_LORA, &def), RADIOLIB_ERR_NONE);
    expect_record("record: MODE=LORA = band default", drv, 12, 125.0f);
}

int main(void)
{
    test_auto_is_radiolib_default_everywhere();
    test_table_bandwidth();
    test_symbol_map();
    test_explicit_beats_auto();
    test_driver_record();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail == 0 ? 0 : 1;
}
