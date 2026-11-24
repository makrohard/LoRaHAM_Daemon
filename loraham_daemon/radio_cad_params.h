#ifndef LORAHAM_RADIO_CAD_PARAMS_H
#define LORAHAM_RADIO_CAD_PARAMS_H

#include <math.h>
#include <stdint.h>

#include <RadioLib.h>

/* --- SX126x CAD parameters -------------------------------------------------- */
/*
 * The three SetCadParams values the SX126x scan uses: the symbol count, detPeak
 * and detMin. The daemon passes them through RadioLib's public
 * SX126x::scanChannel(const ChannelScanConfig_t &). The no-argument
 * scanChannel() leaves all three to RadioLib (4 symbols, SF+13, 10), which is
 * not tuned per spreading factor.
 *
 * RadioCadScanParams holds RadioLib's raw values: sym_code is
 * RADIOLIB_SX126X_CAD_ON_<n>_SYMB (a code, not a count), and
 * RADIOLIB_SX126X_CAD_PARAM_DEFAULT (0xFF) leaves a field to RadioLib.
 */
typedef struct {
    uint8_t sym_code;
    uint8_t det_peak;
    uint8_t det_min;
} RadioCadScanParams;

/* SET CADSYM / CADPEAK / CADMIN as stored: CADSYM as a symbol count, CADPEAK and
 * CADMIN as register values; RADIO_CAD_PARAM_AUTO = the per-SF table. */
#define RADIO_CAD_PARAM_AUTO 0u

typedef struct {
    uint8_t symbols;
    uint8_t det_peak;
    uint8_t det_min;
} RadioCadParamSettings;

/* Symbol count -> RadioLib's code; false for a count the chip has no code for. */
static inline bool radio_cad_sym_code(unsigned count, uint8_t *code)
{
    switch (count) {
    case 1:  *code = RADIOLIB_SX126X_CAD_ON_1_SYMB;  return true;
    case 2:  *code = RADIOLIB_SX126X_CAD_ON_2_SYMB;  return true;
    case 4:  *code = RADIOLIB_SX126X_CAD_ON_4_SYMB;  return true;
    case 8:  *code = RADIOLIB_SX126X_CAD_ON_8_SYMB;  return true;
    case 16: *code = RADIOLIB_SX126X_CAD_ON_16_SYMB; return true;
    default: return false;
    }
}

/* RadioLib's code -> symbol count; 0 for any other byte (0xFF included). */
static inline unsigned radio_cad_sym_count(uint8_t code)
{
    switch (code) {
    case RADIOLIB_SX126X_CAD_ON_1_SYMB:  return 1;
    case RADIOLIB_SX126X_CAD_ON_2_SYMB:  return 2;
    case RADIOLIB_SX126X_CAD_ON_4_SYMB:  return 4;
    case RADIOLIB_SX126X_CAD_ON_8_SYMB:  return 8;
    case RADIOLIB_SX126X_CAD_ON_16_SYMB: return 16;
    default: return 0;
    }
}

/* The bandwidth the per-SF table is for: 125 kHz, compared on the applied value. */
static inline bool radio_cad_table_bw(float bw_khz)
{
    return fabsf(bw_khz - 125.0f) < 0.01f;
}

/*
 * The parameters one scan uses at (sf, bw_khz). An explicit setting is used as
 * given. AUTO takes the table entry for SF7..SF12 at BW 125 kHz; any other SF or
 * bandwidth leaves the field to RadioLib's default.
 *
 * Table: measured on two SX1262 boards at one site, BW 125 kHz, idle channel
 * (sweep S 2848cb4f, pick rule NOTE f6ec9c83, 200-probe guard at another hour,
 * detection leg D with real frames; RESULTS 8a73d7ea). Each entry is the lowest
 * detPeak with 0/40 BUSY at rest on both boards at it and one step above, and
 * guard counts are BUSY/200 on the two boards. RadioLib's detPeak SF+13 reads
 * BUSY on that noise floor at SF8 and up (SF12: 33/40). An entry of
 * RADIOLIB_SX126X_CAD_PARAM_DEFAULT (0xFF) leaves the field to RadioLib, the
 * bytes of the no-argument scanChannel().
 */
static inline RadioCadScanParams radio_cad_sx126x_params(int sf, float bw_khz,
                                                         RadioCadParamSettings set)
{
#define D_ RADIOLIB_SX126X_CAD_PARAM_DEFAULT
    static const RadioCadScanParams bw125[6] = {
        /* SF7: no detection leg D (PREAMBLE range), so RadioLib's default. */
        { D_, D_, D_ },
        /* SF8: S + NOTE + guard 0/1 + D R_b/R_t 1.00/1.00 on both. */
        { RADIOLIB_SX126X_CAD_ON_4_SYMB, 23, 10 },
        /* SF9: S + NOTE + guard 0/1 (26 failed: 0/3) + D 1.00/1.00 on both. */
        { RADIOLIB_SX126X_CAD_ON_4_SYMB, 27, 10 },
        /* SF10: S + NOTE + guard 0/0 + D 1.00/1.00 on both. */
        { RADIOLIB_SX126X_CAD_ON_4_SYMB, 27, 10 },
        /* SF11: S + NOTE + guard 0/0 + D 1.00/1.00 on both. */
        { RADIOLIB_SX126X_CAD_ON_4_SYMB, 28, 10 },
        /* SF12: S + NOTE + guard 1/1 + D 1.00/1.00 on both. */
        { RADIOLIB_SX126X_CAD_ON_4_SYMB, 31, 10 },
    };
    RadioCadScanParams p = { D_, D_, D_ };
#undef D_

    if (radio_cad_table_bw(bw_khz) && sf >= 7 && sf <= 12)
        p = bw125[sf - 7];

    if (set.symbols != RADIO_CAD_PARAM_AUTO)
        radio_cad_sym_code(set.symbols, &p.sym_code);
    if (set.det_peak != RADIO_CAD_PARAM_AUTO)
        p.det_peak = set.det_peak;
    if (set.det_min != RADIO_CAD_PARAM_AUTO)
        p.det_min = set.det_min;
    return p;
}

#endif
