/*
 * The register image of every RadioLib setter path the daemon drives, against
 * a golden image recorded with the RadioLib revision before the current pin.
 *
 * A RadioLib update may change the API (units, structs, return codes) while
 * the daemon's protocol stays the same: SET FREQ=869.525 must still put the
 * same bytes into the chip. Each case boots the REAL Sx127xDriver or
 * Sx1262Driver on the register models (tests/fakes) with a band's boot
 * defaults, applies one SET through the driver's own apply path, re-arms RX as the
 * daemon does after an applied SET (startReceive), and records what the chip
 * was left with:
 *
 *   SX127x: all 128 registers;
 *   SX1262: every opcode sent, with the parameter bytes of its last command,
 *           plus the register windows RadioLib writes;
 *   both:   the setter's return code and the RX interrupt (pin, edge) after
 *           the callback is attached.
 *
 * The boot image of each (chip, band) is recorded in full; a case records only
 * what differs from its boot image (FSK cases: from the FSK boot image). Before
 * a mode switch the SX1262 opcode record is cleared, so an FSK image holds
 * only what the switch and the FSK path sent (a TCXO command the LoRa boot
 * sent cannot stand in for one the FSK path dropped).
 * tests/golden/radio_register_image.inc holds the golden lines; a line that
 * differs is a [FAIL] naming the case and both images. Where a RadioLib
 * revision intentionally changes bytes, the golden line is updated in the same
 * commit as the pin, with the reason in the commit message.
 *
 * Usage: test_radio_register_image            compare with the golden
 *        test_radio_register_image --print F  write the current image to F
 */

#include "../sx127x_driver.h"
#include "../sx1262_driver.h"
#include "../daemon_band.h"
#include "../radio_rf_defaults.h"
#include "../config_policy.h"
#include "fakes/sx127x_register_model.h"
#include "fakes/sx126x_register_model.h"

#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

static const char *const golden[] = {
#include "golden/radio_register_image.inc"
};

static std::vector<std::string> g_lines;

static void rx_callback(void) { }

/* ---- image formatting ---------------------------------------------------- */

static std::string hex_bytes(const uint8_t *b, size_t n)
{
    std::string s;
    char buf[4];
    for (size_t i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "%02X", b[i]);
        s += buf;
    }
    return s;
}

/* An image is a set of "name=value" entries, printed in name order. Against
 * a base image, only the entries that differ are printed, and an entry the
 * base has but this image lacks is printed as "name=(gone)". */
typedef std::map<std::string, std::string> Image;

static std::string image_text(const Image &img, const Image *base)
{
    std::string s;
    for (const auto &e : img) {
        if (base) {
            auto b = base->find(e.first);
            if (b != base->end() && b->second == e.second)
                continue;
        }
        s += " " + e.first + "=" + e.second;
    }
    if (base) {
        for (const auto &b : *base) {
            if (img.find(b.first) == img.end())
                s += " " + b.first + "=(gone)";
        }
    }
    return s;
}

static void record(const std::string &name, int rc, const Image &img,
                   const Image *base)
{
    char head[160];
    snprintf(head, sizeof(head), "%s rc=%d", name.c_str(), rc);
    g_lines.push_back(std::string(head) + image_text(img, base));
}

/* ---- SX127x -------------------------------------------------------------- */

struct Rig127 {
    Sx127xRegisterModel model;
    Module mod;
    Sx127xDriver drv;

    explicit Rig127(bool is_hf)
      : mod(&model, 8, 25, 5, 24), drv(&mod, is_hf, /*high_power*/ true, 100)
    {
        model.reset();
    }

    Image image()
    {
        Image img;
        char name[8];
        for (int a = 1; a < 0x80; a++) {
            uint8_t v = model.peek((uint8_t)a);
            snprintf(name, sizeof(name), "r%02X", a);
            img[name] = hex_bytes(&v, 1);
        }
        char irq[32];
        snprintf(irq, sizeof(irq), "%d/%u", model.attach_pin,
                 (unsigned)model.attach_mode);
        img["irq"] = irq;
        return img;
    }

    void forget_ops() { }   /* the register map is the whole image */
};

/* ---- SX1262 -------------------------------------------------------------- */

/* The registers RadioLib writes on these paths (sync words, CRC, whitening,
 * OCP, IQ, TX clamp, RX gain): a window list keeps the image short. */
static const uint16_t sx126x_reg_windows[][2] = {
    { 0x06B8, 0x06C8 },   /* whitening, CRC init/poly, FSK sync word */
    { 0x0736, 0x0737 },   /* IQ polarity */
    { 0x0740, 0x0742 },   /* LoRa sync word */
    { 0x08AC, 0x08AD },   /* RX gain */
    { 0x08D8, 0x08D9 },   /* TX clamp */
    { 0x08E7, 0x08E8 },   /* OCP */
    { 0x0889, 0x088A },   /* sensitivity (BW500 fix) */
    { 0x0902, 0x0903 },   /* XTA trim */
};

struct Rig1262 {
    Sx126xRegisterModel model;
    Module mod;
    Sx1262Driver drv;

    explicit Rig1262(float tcxo)
      : mod(&model, 21, 16, 18, 20), drv(&mod, tcxo, -1, 100, /*high_power*/ true)
    {
        model.reset();
    }

    Image image()
    {
        Image img;
        char name[16];
        /* Every opcode sent since reset, with its last parameter bytes: a
         * command a RadioLib revision adds or drops on a path shows up. */
        for (int op = 0; op < 256; op++) {
            if (!model.seen[op])
                continue;
            snprintf(name, sizeof(name), "op%02X", op);
            img[name] = hex_bytes(model.op_bytes[op], model.op_len[op]);
        }
        for (const auto &w : sx126x_reg_windows) {
            for (uint16_t a = w[0]; a < w[1]; a++) {
                uint8_t v = model.peek(a);
                snprintf(name, sizeof(name), "r%04X", a);
                img[name] = hex_bytes(&v, 1);
            }
        }
        char irq[32];
        snprintf(irq, sizeof(irq), "%d/%u", model.attach_pin,
                 (unsigned)model.attach_mode);
        img["irq"] = irq;
        return img;
    }

    void forget_ops() { model.forget_ops(); }
};

/* ---- cases --------------------------------------------------------------- */

struct Set { const char *key; const char *val; };

static std::vector<Set> lora_sets(int band)
{
    std::vector<Set> v = {
        {"SF", "7"}, {"SF", "8"}, {"SF", "9"}, {"SF", "10"}, {"SF", "11"}, {"SF", "12"},
        {"BW", "7.8"}, {"BW", "10.4"}, {"BW", "15.6"}, {"BW", "20.8"}, {"BW", "31.25"},
        {"BW", "41.7"}, {"BW", "62.5"}, {"BW", "125"}, {"BW", "250"}, {"BW", "500"},
        {"CR", "5"}, {"CR", "6"}, {"CR", "7"}, {"CR", "8"},
        {"CRC", "0"}, {"CRC", "1"},
        {"PREAMBLE", "6"}, {"PREAMBLE", "8"}, {"PREAMBLE", "16"}, {"PREAMBLE", "512"},
        {"PREAMBLE", "65535"},
        {"SYNC", "0x12"}, {"SYNC", "0x34"}, {"SYNC", "0x2B"},
        {"LDRO", "auto"}, {"LDRO", "0"}, {"LDRO", "1"},
        {"POWER", "0"}, {"POWER", "2"}, {"POWER", "5"}, {"POWER", "10"}, {"POWER", "14"},
        {"POWER", "17"}, {"POWER", "20"}, {"POWER", "22"},
    };
    static const char *f433[] = {"430", "433.175", "433.65", "433.775", "433.9",
                                 "434", "439.9125", "440"};
    static const char *f868[] = {"863", "867.875", "868.1", "869.4625", "869.525",
                                 "869.618", "870"};
    static const char *f915[] = {"902", "906.875", "915", "928"};
    const char *const *f = band == 433 ? f433 : band == 868 ? f868 : f915;
    size_t n = band == 433 ? 8 : band == 868 ? 7 : 4;
    for (size_t i = 0; i < n; i++)
        v.push_back({"FREQ", f[i]});
    return v;
}

static std::vector<Set> fsk_sets(int band, bool sx1262)
{
    std::vector<Set> v = {
        {"BR", "0.6"}, {"BR", "1.2"}, {"BR", "2.4"}, {"BR", "4.8"}, {"BR", "9.6"},
        {"BR", "19.2"}, {"BR", "38.4"}, {"BR", "50"}, {"BR", "100"}, {"BR", "150"},
        {"BR", "250"}, {"BR", "300"},
        {"FREQDEV", "0.6"}, {"FREQDEV", "2.5"}, {"FREQDEV", "5"}, {"FREQDEV", "10"},
        {"FREQDEV", "25"}, {"FREQDEV", "50"}, {"FREQDEV", "100"}, {"FREQDEV", "200"},
        {"SHAPING", "off"}, {"SHAPING", "0.3"}, {"SHAPING", "0.5"}, {"SHAPING", "0.7"},
        {"SHAPING", "1.0"},
        {"ENCODING", "0"}, {"ENCODING", "1"}, {"ENCODING", "2"},
        {"PREAMBLE", "0"}, {"PREAMBLE", "16"}, {"PREAMBLE", "32"}, {"PREAMBLE", "2048"},
        {"SYNC", "0x12"}, {"SYNC", "0x2DD4"},
        {"POWER", "2"}, {"POWER", "10"}, {"POWER", "17"},
        {"OOK", "0"}, {"OOK", "1"},
    };
    static const char *rx127[] = {"2.6", "3.1", "3.9", "5.2", "6.3", "7.8", "10.4",
                                  "12.5", "15.6", "20.8", "25", "31.25", "31.3", "41.7",
                                  "50", "62.5", "83.3", "100", "125", "166.7", "200",
                                  "250"};
    static const char *rx126[] = {"4.8", "5.8", "7.3", "9.7", "11.7", "14.6", "19.5",
                                  "23.4", "29.3", "39", "46.9", "58.6", "78.2", "93.8",
                                  "117.3", "156.2", "187.2", "234.3", "312", "373.6",
                                  "467"};
    if (sx1262) {
        for (const char *r : rx126)
            v.push_back({"RXBW", r});
    } else {
        for (const char *r : rx127)
            v.push_back({"RXBW", r});
    }
    v.push_back({"FREQ", band == 433 ? "433.775" : band == 868 ? "869.525" : "915"});
    v.push_back({"FREQ", band == 433 ? "439.9125" : band == 868 ? "868.1" : "906.875"});
    return v;
}

static const RadioRfDefaults *band_defaults(int band)
{
    daemon_band_resolve(band == 433 ? RADIO_BAND_433
                        : band == 868 ? RADIO_BAND_868 : RADIO_BAND_915);
    return daemon_band()->rf_defaults;
}

/* The chip's answer to the next CAD: done, with or without a detection. */
static void cad_script(Sx127xRegisterModel &m, bool detected)
{
    m.cad_reads_until_done = 0;
    m.cad_detects = detected;
}

static void cad_script(Sx126xRegisterModel &m, bool detected)
{
    m.irq_high_pin = 16;   /* DIO1 of the rig: the CAD wait ends at once */
    m.irq_status = (uint16_t)(detected ? 0x0180 : 0x0080);   /* CadDone (+ CadDetected) */
}

template <class Rig, class Make>
static void run_chip(const std::string &chip, int band, bool sx1262, Make make)
{
    const RadioRfDefaults *def = band_defaults(band);
    std::string base_name = chip + "/" + std::to_string(band);

    Image lora_base, fsk_base;
    {
        Rig *r = make();
        int rc = r->drv.begin(def);
        r->drv.setPacketReceivedAction(rx_callback);
        record(base_name + "/boot-begin", rc, r->image(), nullptr);
        rc = r->drv.startReceive();
        lora_base = r->image();
        record(base_name + "/boot", rc, lora_base, nullptr);

        r->forget_ops();
        rc = r->drv.switchMode(RADIO_MODE_FSK, def);
        r->drv.setPacketReceivedAction(rx_callback);
        r->drv.startReceive();
        fsk_base = r->image();
        record(base_name + "/fsk", rc, fsk_base, nullptr);

        r->forget_ops();
        rc = r->drv.switchMode(RADIO_MODE_LORA, def);
        r->drv.setPacketReceivedAction(rx_callback);
        r->drv.startReceive();
        record(base_name + "/fsk-lora", rc, r->image(), &lora_base);

        r->drv.clearPacketReceivedAction();
        char det[24];
        snprintf(det, sizeof(det), " detach=%d", r->model.detach_pin);
        g_lines.push_back(base_name + "/clear-irq" + det);
        delete r;
    }

    /* CAD: the scan the TX worker runs before a send, free and detected,
     * then the RX re-arm after it. */
    for (bool detected : {false, true}) {
        Rig *r = make();
        r->drv.begin(def);
        r->drv.setPacketReceivedAction(rx_callback);
        r->drv.startReceive();
        cad_script(r->model, detected);
        int rc = r->drv.scanChannel();
        r->drv.startReceive();
        record(base_name + (detected ? "/cad-detected" : "/cad-free"), rc, r->image(),
               &lora_base);
        delete r;
    }

    for (const Set &s : lora_sets(band)) {
        Rig *r = make();
        r->drv.begin(def);
        r->drv.setPacketReceivedAction(rx_callback);
        int rc = r->drv.applyLoraParam("T", s.key, s.val);
        r->drv.startReceive();
        record(base_name + "/lora/" + s.key + "=" + s.val, rc, r->image(), &lora_base);
        delete r;
    }

    for (const Set &s : fsk_sets(band, sx1262)) {
        Rig *r = make();
        r->drv.begin(def);
        r->forget_ops();
        r->drv.switchMode(RADIO_MODE_FSK, def);
        r->drv.setPacketReceivedAction(rx_callback);
        int rc = r->drv.applyFskParam("T", s.key, s.val);
        r->drv.startReceive();
        record(base_name + "/fsk/" + s.key + "=" + s.val, rc, r->image(), &fsk_base);
        delete r;
    }
}

static void build_image(void)
{
    run_chip<Rig127>("sx1278", 433, false, [] { return new Rig127(false); });
    run_chip<Rig127>("rfm95", 868, false, [] { return new Rig127(true); });
    run_chip<Rig127>("rfm95", 915, false, [] { return new Rig127(true); });
    for (int band : {433, 868, 915}) {
        run_chip<Rig1262>("sx1262-tcxo18", band, true, [] { return new Rig1262(1.8f); });
        run_chip<Rig1262>("sx1262-xtal", band, true, [] { return new Rig1262(0.0f); });
    }
}

int main(int argc, char **argv)
{
    /* The driver apply paths print inline fragments; keep them apart from
     * the verdict lines. */
    config_policy_set_sx1262_power_max(22);
    build_image();
    printf("\n");

    if (argc == 3 && strcmp(argv[1], "--print") == 0) {
        FILE *f = fopen(argv[2], "w");
        if (!f)
            return 2;
        fprintf(f, "/* Generated by test_radio_register_image --print with RadioLib %d.%d.%d.%d; see the test. */\n",
                RADIOLIB_VERSION_MAJOR, RADIOLIB_VERSION_MINOR, RADIOLIB_VERSION_PATCH,
                RADIOLIB_VERSION_EXTRA);
        for (const std::string &l : g_lines)
            fprintf(f, "\"%s\",\n", l.c_str());
        fclose(f);
        printf("wrote %zu lines to %s\n", g_lines.size(), argv[2]);
        return 0;
    }

    const size_t n_golden = sizeof(golden) / sizeof(golden[0]);
    int ok = 0, fail = 0;
    for (size_t i = 0; i < g_lines.size() || i < n_golden; i++) {
        const char *want = i < n_golden ? golden[i] : "(none)";
        const char *got = i < g_lines.size() ? g_lines[i].c_str() : "(none)";
        if (strcmp(want, got) == 0) {
            ok++;
        } else {
            fail++;
            printf("[FAIL] image line %zu:\n  golden: %s\n  now:    %s\n", i, want, got);
        }
    }
    printf("[%s] register image: %d lines identical, %d differ (golden %zu, now %zu)\n",
           fail ? "FAIL" : " OK ", ok, fail, n_golden, g_lines.size());
    printf("Summary: %d passed, %d failed\n", fail ? 0 : 1, fail ? 1 : 0);
    return fail ? 1 : 0;
}
