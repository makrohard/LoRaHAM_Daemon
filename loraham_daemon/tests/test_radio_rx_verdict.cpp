/*
 * The receive verdict of every RX path the daemon reads, against a golden
 * recorded with the RadioLib revision before the current pin.
 *
 * The daemon delivers a received frame only when readData() returns
 * RADIOLIB_ERR_NONE (daemon_rx_read_ok()); every other code is a drop. So the
 * verdict RadioLib gives for a frame -- good, payload CRC error, damaged
 * header, a command error, a timeout -- decides what the clients see, and a
 * RadioLib update that changes it changes the daemon. Each case boots the REAL
 * driver on the register model with a band's boot defaults (FSK cases switch
 * to FSK), arms RX as the daemon does, puts one received frame and its flags
 * into the model, and reads it the daemon's way (getPacketLength(), then
 * readData() with that length). Recorded per case: the length, readData()'s
 * return code, the bytes it returned, and what it left in the chip's flag
 * registers (SX127x RegIrqFlags / RegIrqFlags2; SX1262 the ClearIrqStatus
 * bytes), plus the CRC configuration the verdict depends on (SX127x
 * RegModemConfig / RegPacketConfig1 bytes).
 *
 * tests/golden/radio_rx_verdict.inc holds the golden lines; a line that
 * differs is a [FAIL] naming the case and both lines.
 *
 * Usage: test_radio_rx_verdict            compare with the golden
 *        test_radio_rx_verdict --print F  write the current lines to F
 */

#include "../sx127x_driver.h"
#include "../sx1262_driver.h"
#include "../daemon_band.h"
#include "../radio_rf_defaults.h"
#include "fakes/sx127x_register_model.h"
#include "fakes/sx126x_register_model.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

static const char *const golden[] = {
#include "golden/radio_rx_verdict.inc"
};

static std::vector<std::string> g_lines;

static void rx_callback(void) { }

static const uint8_t FRAME[] = { 'H', 'E', 'L', 'L', 'O' };
static const size_t FRAME_LEN = sizeof(FRAME);

static std::string hex_bytes(const uint8_t *b, size_t n)
{
    std::string s;
    char buf[4];
    for (size_t i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "%02X", b[i]);
        s += buf;
    }
    return s.empty() ? "-" : s;
}

static const RadioRfDefaults *band_defaults(int band)
{
    daemon_band_resolve(band == 433 ? RADIO_BAND_433 : RADIO_BAND_868);
    return daemon_band()->rf_defaults;
}

/* The daemon's read: length first, then readData() with that length. */
template <class Drv>
static std::string read_frame(Drv &drv, size_t want)
{
    size_t len = drv.getPacketLength();
    uint8_t buf[256];
    memset(buf, 0xEE, sizeof(buf));
    size_t n = want ? want : len;
    if (n > sizeof(buf))
        n = sizeof(buf);
    int rc = drv.readData(buf, n);
    char head[64];
    snprintf(head, sizeof(head), "len=%zu rc=%d data=", len, rc);
    return std::string(head) + hex_bytes(buf, n);
}

/* ---- SX127x -------------------------------------------------------------- */

static const uint8_t R127_IRQ_FLAGS   = 0x12;
static const uint8_t R127_RX_NB_BYTES = 0x13;
static const uint8_t R127_HOP_CHANNEL = 0x1C;   /* bit 6: CrcOnPayload of the received header */
static const uint8_t R127_MODEM_CFG1  = 0x1D;
static const uint8_t R127_MODEM_CFG2  = 0x1E;   /* bit 2: RxPayloadCrcOn */
static const uint8_t R127_PKT_CFG1    = 0x30;   /* FSK: bit 4 CrcOn, bit 3 CrcAutoClearOff */
static const uint8_t R127_IRQ_FLAGS1  = 0x3E;
static const uint8_t R127_IRQ_FLAGS2  = 0x3F;   /* FSK: bit 2 PayloadReady, bit 1 CrcOk */

struct Rig127 {
    Sx127xRegisterModel model;
    Module mod;
    Sx127xDriver drv;

    explicit Rig127(bool is_hf) : mod(&model, 8, 25, 5, 24), drv(&mod, is_hf, false, 100)
    {
        model.reset();
    }
};

struct Lora127Case {
    const char *name;
    const char *crc_set;   /* "0"/"1": SET CRC before the frame; NULL: boot default (on) */
    uint8_t irq;           /* RegIrqFlags: RxDone 0x40, PayloadCrcError 0x20, ValidHeader 0x10 */
    uint8_t hop;           /* RegHopChannel: 0x40 = the header says CRC on */
    size_t want;           /* 0 = the packet length */
};

static void sx127x_lora(const char *chip, bool is_hf, int band)
{
    static const Lora127Case cases[] = {
        { "good",                       NULL, 0x50, 0x40, 0 },
        { "payload-crc-error",          NULL, 0x70, 0x40, 0 },
        { "header-says-no-crc",         NULL, 0x50, 0x00, 0 },
        { "header-says-no-crc+crc-err", NULL, 0x70, 0x00, 0 },
        { "crc-off/good",               "0",  0x50, 0x00, 0 },
        { "crc-off/header-says-crc",    "0",  0x50, 0x40, 0 },
        { "crc-off/payload-crc-error",  "0",  0x70, 0x00, 0 },
        { "crc-on-set/header-says-no-crc", "1", 0x50, 0x00, 0 },
        { "short-read-3-of-5",          NULL, 0x50, 0x40, 3 },
    };
    const RadioRfDefaults *def = band_defaults(band);
    for (const Lora127Case &c : cases) {
        Rig127 r(is_hf);
        r.drv.begin(def);
        if (c.crc_set)
            r.drv.applyLoraParam("T", "CRC", c.crc_set);
        r.drv.setPacketReceivedAction(rx_callback);
        r.drv.startReceive();
        r.model.poke(R127_RX_NB_BYTES, (uint8_t)FRAME_LEN);
        r.model.poke(R127_HOP_CHANNEL, c.hop);
        r.model.poke(R127_IRQ_FLAGS, c.irq);
        r.model.load_rx_fifo(FRAME, FRAME_LEN);
        std::string got = read_frame(r.drv, c.want);
        char tail[96];
        snprintf(tail, sizeof(tail), " irq-after=%02X cfg=%02X%02X",
                 r.model.peek(R127_IRQ_FLAGS), r.model.peek(R127_MODEM_CFG1),
                 r.model.peek(R127_MODEM_CFG2));
        g_lines.push_back(std::string(chip) + "/" + std::to_string(band) + "/lora/" + c.name +
                          " " + got + tail);
    }
}

struct Fsk127Case {
    const char *name;
    uint8_t flags2;        /* RegIrqFlags2: PayloadReady 0x04, CrcOk 0x02 */
};

static void sx127x_fsk(const char *chip, bool is_hf, int band)
{
    static const Fsk127Case cases[] = {
        { "payload-ready+crc-ok", 0x06 },
        { "payload-ready+crc-bad", 0x04 },
    };
    const RadioRfDefaults *def = band_defaults(band);
    for (const Fsk127Case &c : cases) {
        Rig127 r(is_hf);
        r.drv.begin(def);
        r.drv.switchMode(RADIO_MODE_FSK, def);
        r.drv.setPacketReceivedAction(rx_callback);
        r.drv.startReceive();
        /* Variable length (beginFSK's default packet format): the length byte
         * first, then the payload. */
        uint8_t fifo[1 + sizeof(FRAME)];
        fifo[0] = (uint8_t)FRAME_LEN;
        memcpy(fifo + 1, FRAME, FRAME_LEN);
        r.model.poke(R127_IRQ_FLAGS2, c.flags2);
        r.model.load_rx_fifo(fifo, sizeof(fifo));
        std::string got = read_frame(r.drv, 0);
        char tail[96];
        snprintf(tail, sizeof(tail), " flags-after=%02X%02X pktcfg1=%02X",
                 r.model.peek(R127_IRQ_FLAGS1), r.model.peek(R127_IRQ_FLAGS2),
                 r.model.peek(R127_PKT_CFG1));
        g_lines.push_back(std::string(chip) + "/" + std::to_string(band) + "/fsk/" + c.name +
                          " " + got + tail);
    }
}

/* ---- SX1262 -------------------------------------------------------------- */

struct Rig1262 {
    Sx126xRegisterModel model;
    Module mod;
    Sx1262Driver drv;

    Rig1262() : mod(&model, 21, 16, 18, 20), drv(&mod, 1.8f, -1, 100, false) { model.reset(); }
};

struct Case1262 {
    const char *name;
    const char *crc_set;   /* LoRa only: "0"/"1" before the frame; NULL: boot default */
    uint16_t irq;          /* RxDone 0x0002, HeaderValid 0x0010, HeaderErr 0x0020, CrcErr 0x0040, Timeout 0x0200 */
    uint8_t status;        /* reply status byte; 0x22 = OK */
};

static void sx1262_rx(bool fsk)
{
    static const Case1262 lora_cases[] = {
        { "good",                    NULL, 0x0012, 0x22 },
        { "crc-error",               NULL, 0x0052, 0x22 },
        { "header-error",            NULL, 0x0022, 0x22 },
        { "header-error+valid",      NULL, 0x0032, 0x22 },
        { "crc-off/good",            "0",  0x0012, 0x22 },
        { "crc-off/crc-error",       "0",  0x0052, 0x22 },
        { "command-failed-status",   NULL, 0x0012, 0x2A },
        { "command-timeout+irq-timeout", NULL, 0x0200, 0x26 },
        { "command-timeout",         NULL, 0x0012, 0x26 },
    };
    static const Case1262 fsk_cases[] = {
        { "good",                    NULL, 0x0002, 0x22 },
        { "crc-error",               NULL, 0x0042, 0x22 },
        { "command-failed-status",   NULL, 0x0002, 0x2A },
    };
    const RadioRfDefaults *def = band_defaults(868);
    const Case1262 *cases = fsk ? fsk_cases : lora_cases;
    size_t n = fsk ? sizeof(fsk_cases) / sizeof(fsk_cases[0])
                   : sizeof(lora_cases) / sizeof(lora_cases[0]);
    for (size_t i = 0; i < n; i++) {
        const Case1262 &c = cases[i];
        Rig1262 r;
        r.drv.begin(def);
        if (fsk)
            r.drv.switchMode(RADIO_MODE_FSK, def);
        if (c.crc_set)
            r.drv.applyLoraParam("T", "CRC", c.crc_set);
        r.drv.setPacketReceivedAction(rx_callback);
        r.drv.startReceive();
        memcpy(r.model.rx_buf, FRAME, FRAME_LEN);
        r.model.rx_len = (uint8_t)FRAME_LEN;
        r.model.irq_status = c.irq;
        r.model.seen[0x02] = false;   /* ClearIrqStatus: only readData()'s own */
        r.model.status = c.status;
        std::string got = read_frame(r.drv, 0);
        r.model.status = Sx126xRegisterModel::STATUS_OK;
        std::string clr = r.model.seen[0x02] ? hex_bytes(r.model.op_bytes[0x02], r.model.op_len[0x02])
                                              : std::string("-");
        g_lines.push_back(std::string("sx1262-tcxo18/868/") + (fsk ? "fsk/" : "lora/") + c.name +
                          " " + got + " clear-irq=" + clr);
    }
}

int main(int argc, char **argv)
{
    sx127x_lora("sx1278", false, 433);
    sx127x_lora("rfm95", true, 868);
    sx127x_fsk("sx1278", false, 433);
    sx127x_fsk("rfm95", true, 868);
    sx1262_rx(false);
    sx1262_rx(true);
    printf("\n");

    if (argc == 3 && strcmp(argv[1], "--print") == 0) {
        FILE *f = fopen(argv[2], "w");
        if (!f)
            return 2;
        fprintf(f, "/* Generated by test_radio_rx_verdict --print with RadioLib %d.%d.%d.%d; see the test. */\n",
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
            printf("[ OK ] %s\n", got);
        } else {
            fail++;
            printf("[FAIL] rx verdict line %zu:\n  golden: %s\n  now:    %s\n", i, want, got);
        }
    }
    printf("Summary: %d passed, %d failed\n", ok, fail);
    return fail ? 1 : 0;
}
