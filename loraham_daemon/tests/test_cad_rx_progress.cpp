/*
 * A CAD probe must not cut off a frame that is still arriving.
 *
 * The REAL Sx1262Driver and Sx127xDriver against the REAL pinned RadioLib,
 * with tests/fakes/sx126x_register_model.h and sx127x_register_model.h where
 * the chips would be. Both probe sites (the MANAGED gate's radio_cad_probe and
 * GET CHANNEL's radio_cad_try_probe) are driven with the chip showing a frame
 * in progress -- SX126x: the latched IRQ status, SX127x: the live RegModemStat
 * -- and must answer BUSY without stopping the receiver.
 *
 * Uses only interfaces that predate the repair (the probe entries,
 * driver->startReceive()), so the same file runs against the earlier sources
 * (red-before).
 */

#include "../radio_cad.h"
#include "../sx1262_driver.h"
#include "../sx127x_driver.h"
#include "fakes/sx126x_register_model.h"
#include "fakes/sx127x_register_model.h"

#include <stdio.h>
#include <string>

static int g_ok = 0;
static int g_fail = 0;

static void expect_true(const std::string &name, bool cond, const std::string &detail)
{
    if (cond) {
        g_ok++;
        printf("[ OK ] %s\n", name.c_str());
    } else {
        g_fail++;
        printf("[FAIL] %s: %s\n", name.c_str(), detail.c_str());
    }
}

static void expect_int(const std::string &name, long got, long want)
{
    expect_true(name, got == want,
                "expected " + std::to_string(want) + ", got " + std::to_string(got));
}

static void rx_callback(void) { }

static RadioCadProbeResult probe(RadioController *ctrl, bool try_probe)
{
    return try_probe ? radio_cad_try_probe(ctrl) : radio_cad_probe(ctrl);
}

static const char *site(bool try_probe)
{
    return try_probe ? "GET CHANNEL" : "managed";
}

/* --- SX1262 ---------------------------------------------------------------- */

static const int PIN_CS = 21, PIN_IRQ = 16, PIN_RST = 18, PIN_BUSY = 20;

static const uint8_t OP_CLEAR_IRQ_STATUS = 0x02;
static const uint8_t OP_SET_STANDBY = 0x80;
static const uint8_t OP_SET_RX = 0x82;
static const uint8_t OP_SET_CAD = 0xC5;

struct Sx1262Rig {
    Sx126xRegisterModel model;
    Module mod;
    RadioController ctrl;

    Sx1262Rig() : mod(&model, PIN_CS, PIN_IRQ, PIN_RST, PIN_BUSY)
    {
        RadioRfDefaults def{};

        def.freq_mhz = 868.3f;
        def.spreading_factor = 8;
        def.bandwidth_khz = 125.0f;
        def.sync_word = 0x12;
        def.preamble_len = 8;
        def.coding_rate = 5;
        def.crc_on = true;
        def.ldro = -1;
        def.power_dbm = 10;

        model.reset();
        model.irq_high_pin = PIN_IRQ;   /* a scan, if one runs, ends at once */
        radio_controller_init(&ctrl, RADIO_BAND_868, "T", true, rx_callback, -1);
        ctrl.chip_family = DAEMON_CHIP_FAMILY_SX1262;
        ctrl.driver.reset(sx1262_driver_create(&mod, 1.8f, -1, 100, false));
        ctrl.driver->begin(&def);
        ctrl.health = RADIO_HEALTH_READY;
        model.forget_ops();
    }
};

/* Every re-arm goes through driver->startReceive(): it must latch
 * PREAMBLE_DETECTED on top of RadioLib's default RX set, and DIO1 must still
 * rise on RxDone only. */
static void test_sx1262_rx_latches_the_preamble(void)
{
    Sx1262Rig rig;
    const uint16_t want_irq = RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_TIMEOUT |
                              RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_VALID |
                              RADIOLIB_SX126X_IRQ_HEADER_ERR |
                              RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED;

    expect_int("sx1262 startReceive OK", rig.ctrl.driver->startReceive(), RADIOLIB_ERR_NONE);
    const uint8_t *b = rig.model.dio_irq;
    const uint16_t irq = (uint16_t)((b[0] << 8) | b[1]);
    const uint16_t dio1 = (uint16_t)((b[2] << 8) | b[3]);

    expect_int("sx1262 RX IRQ mask = RadioLib default + PREAMBLE_DETECTED", irq, want_irq);
    expect_int("sx1262 RX DIO1 = RxDone only", dio1, RADIOLIB_SX126X_IRQ_RX_DONE);
    expect_true("sx1262 startReceive entered RX", rig.model.seen[OP_SET_RX], "no SetRx");
}

/* A frame in progress (preamble, or header) on either probe site: BUSY, and
 * no standby, IRQ clear, CAD or re-arm reaches the chip. */
static void test_sx1262_frame_in_progress_is_left_alone(void)
{
    static const struct {
        const char *name;
        uint16_t irq;
    } cases[] = {
        { "preamble", RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED },
        { "header", RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_HEADER_VALID },
        { "header bit alone", RADIOLIB_SX126X_IRQ_HEADER_VALID },
        /* A header error raises no interrupt and stays latched; a later
         * frame's preamble latches beside it. A stale pair alone looks the
         * same, and the probe's stage-(i) bound releases it (residual: a new
         * frame starting inside that window, its header not yet valid when
         * the bound runs out, can still be cut). */
        { "preamble beside a stale header error",
          RADIOLIB_SX126X_IRQ_HEADER_ERR | RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED },
        { "header beside a stale header error",
          RADIOLIB_SX126X_IRQ_HEADER_ERR | RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED |
              RADIOLIB_SX126X_IRQ_HEADER_VALID },
    };

    for (const auto &c : cases) {
        for (int t = 0; t < 2; t++) {
            Sx1262Rig rig;
            const std::string n = std::string("sx1262 ") + site(t) + ", " + c.name + ": ";

            rig.model.irq_status = c.irq;
            RadioCadProbeResult r = probe(&rig.ctrl, t != 0);

            expect_int(n + "BUSY", r.status, RADIO_CAD_PROBE_BUSY);
            expect_int(n + "no scan", r.scan_ran, 0);
            expect_true(n + "no SetStandby", !rig.model.seen[OP_SET_STANDBY], "standby sent");
            expect_true(n + "no ClearIrqStatus", !rig.model.seen[OP_CLEAR_IRQ_STATUS], "IRQ cleared");
            expect_true(n + "no SetCad", !rig.model.seen[OP_SET_CAD], "CAD started");
            expect_true(n + "no SetRx", !rig.model.seen[OP_SET_RX], "RX re-armed");
            expect_int(n + "received not set", rig.ctrl.received.load() ? 1 : 0, 0);
        }
    }
}

/* The same bits next to RX_DONE mean that frame is complete: the pending-packet
 * path as before (standby, BUSY, received). A header error alone is no frame:
 * the normal probe (standby, CAD, and the restore clears it). */
static void test_sx1262_ended_frame_is_not_in_progress(void)
{
    {
        Sx1262Rig rig;

        rig.model.irq_status = RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED |
                               RADIOLIB_SX126X_IRQ_HEADER_VALID | RADIOLIB_SX126X_IRQ_RX_DONE;
        RadioCadProbeResult r = radio_cad_probe(&rig.ctrl);
        expect_int("sx1262 RxDone: BUSY", r.status, RADIO_CAD_PROBE_BUSY);
        expect_true("sx1262 RxDone: quiesced first", rig.model.seen[OP_SET_STANDBY], "no standby");
        expect_int("sx1262 RxDone: received set for the drain",
                   rig.ctrl.received.load() ? 1 : 0, 1);
        expect_true("sx1262 RxDone: no CAD", !rig.model.seen[OP_SET_CAD], "CAD started");
        expect_true("sx1262 RxDone: IRQ flags left for the drain",
                    !rig.model.seen[OP_CLEAR_IRQ_STATUS], "IRQ cleared");
    }
    {
        /* RxDone beside a stale header error: the same pending-packet path. */
        Sx1262Rig rig;

        rig.model.irq_status = RADIOLIB_SX126X_IRQ_HEADER_ERR |
                               RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_RX_DONE;
        RadioCadProbeResult r = radio_cad_probe(&rig.ctrl);
        expect_int("sx1262 RxDone beside a header error: BUSY", r.status, RADIO_CAD_PROBE_BUSY);
        expect_int("sx1262 RxDone beside a header error: received set for the drain",
                   rig.ctrl.received.load() ? 1 : 0, 1);
        expect_true("sx1262 RxDone beside a header error: IRQ flags left for the drain",
                    !rig.model.seen[OP_CLEAR_IRQ_STATUS], "IRQ cleared");
        expect_true("sx1262 RxDone beside a header error: no CAD",
                    !rig.model.seen[OP_SET_CAD], "CAD started");
    }
    {
        Sx1262Rig rig;

        rig.model.irq_status = RADIOLIB_SX126X_IRQ_HEADER_ERR;
        RadioCadProbeResult r = radio_cad_probe(&rig.ctrl);
        expect_int("sx1262 header error: normal probe scanned", r.scan_ran, 1);
        expect_true("sx1262 header error: standby", rig.model.seen[OP_SET_STANDBY], "no standby");
        expect_true("sx1262 header error: CAD", rig.model.seen[OP_SET_CAD], "no CAD");
        expect_true("sx1262 header error: re-armed", rig.model.seen[OP_SET_RX], "no SetRx");
        expect_true("sx1262 header error: flags cleared", rig.model.seen[OP_CLEAR_IRQ_STATUS],
                    "no ClearIrqStatus");

        /* The next frame's preamble, after that probe: in progress. */
        rig.model.forget_ops();
        rig.model.irq_status = RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED;
        r = radio_cad_probe(&rig.ctrl);
        expect_int("sx1262 header error, then a preamble: BUSY", r.status, RADIO_CAD_PROBE_BUSY);
        expect_true("sx1262 header error, then a preamble: no SetStandby",
                    !rig.model.seen[OP_SET_STANDBY], "standby sent");
    }
}

/* FSK: every re-arm keeps RadioLib's own no-argument RX (its default IRQ set,
 * no PREAMBLE_DETECTED), the same bytes a plain RadioLib SX1262 sends. */
static void test_sx1262_fsk_rx_is_radiolib_default(void)
{
    Sx1262Rig rig;
    RadioRfDefaults def{};
    const uint16_t want_irq = RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_TIMEOUT |
                              RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_VALID |
                              RADIOLIB_SX126X_IRQ_HEADER_ERR;

    def.freq_mhz = 868.3f;
    rig.model.set_packet_type(RADIOLIB_SX126X_PACKET_TYPE_GFSK);
    expect_int("sx1262 FSK switch OK", rig.ctrl.driver->switchMode(RADIO_MODE_FSK, &def),
               RADIOLIB_ERR_NONE);
    rig.model.forget_ops();
    expect_int("sx1262 FSK startReceive OK", rig.ctrl.driver->startReceive(), RADIOLIB_ERR_NONE);
    const uint8_t *b = rig.model.dio_irq;
    const uint16_t irq = (uint16_t)((b[0] << 8) | b[1]);
    const uint16_t dio1 = (uint16_t)((b[2] << 8) | b[3]);

    expect_int("sx1262 FSK RX IRQ mask = RadioLib default", irq, want_irq);
    expect_int("sx1262 FSK RX DIO1 = RxDone only", dio1, RADIOLIB_SX126X_IRQ_RX_DONE);
    expect_true("sx1262 FSK startReceive entered RX", rig.model.seen[OP_SET_RX], "no SetRx");
}

/* FSK: the probe never reaches the chip, as before. */
static void test_sx1262_fsk_unchanged(void)
{
    Sx1262Rig rig;

    rig.ctrl.mode = RADIO_MODE_FSK;
    rig.model.irq_status = RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED;
    RadioCadProbeResult r = radio_cad_probe(&rig.ctrl);
    expect_int("sx1262 FSK: UNAVAILABLE", r.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_true("sx1262 FSK: no SetStandby", !rig.model.seen[OP_SET_STANDBY], "standby sent");
    expect_true("sx1262 FSK: no SetCad", !rig.model.seen[OP_SET_CAD], "CAD started");
}

/* --- SX127x ---------------------------------------------------------------- */

static const uint8_t REG_MODEM_STAT = 0x18;

/* LoRa modem in RX continuous, the receiver running as between probes (begin()
 * itself is not run, as in test_sx127x_cad_register), so a probe's standby is a
 * transition the model counts. */
struct Sx127xRig {
    Sx127xRegisterModel model;
    Module mod;
    RadioController ctrl;

    Sx127xRig() : mod(&model, 8, 25, 5, 24)
    {
        model.reset();
        model.poke(Sx127xRegisterModel::REG_OP_MODE,
                   Sx127xRegisterModel::LONG_RANGE | 0x05);   /* RX continuous */
        radio_controller_init(&ctrl, RADIO_BAND_433, "T", false, rx_callback, -1);
        ctrl.chip_family = DAEMON_CHIP_FAMILY_SX127X;
        ctrl.driver.reset(new Sx127xDriver(&mod, false));
        ctrl.health = RADIO_HEALTH_READY;
        model.cad_reads_until_done = 0;   /* a scan, if one runs, is FREE at once */
        model.cad_detects = false;
    }
};

/* RegModemStat bits 1 (signal synchronized), 2 (RX on-going) and 3 (header
 * valid): a frame in progress, from the preamble lock on. */
static void test_sx127x_frame_in_progress_is_left_alone(void)
{
    static const struct {
        const char *name;
        uint8_t stat;
    } cases[] = {
        { "signal synchronized", 0x02 },
        { "RX on-going", 0x04 },
        { "header valid", 0x0B },
    };

    for (const auto &c : cases) {
        for (int t = 0; t < 2; t++) {
            Sx127xRig rig;
            const std::string n = std::string("sx127x ") + site(t) + ", " + c.name + ": ";

            rig.model.poke(REG_MODEM_STAT, c.stat);
            RadioCadProbeResult r = probe(&rig.ctrl, t != 0);

            expect_int(n + "BUSY", r.status, RADIO_CAD_PROBE_BUSY);
            expect_int(n + "no scan", r.scan_ran, 0);
            expect_int(n + "no standby", rig.model.standby_entries, 0);
            expect_int(n + "no CAD", rig.model.cad_entries, 0);
            expect_int(n + "no IRQ clear", rig.model.irq_flag_clears, 0);
            expect_int(n + "received not set", rig.ctrl.received.load() ? 1 : 0, 0);
        }
    }
}

/* Bit 0 (signal detected) alone, and modem clear: no frame, a normal probe. */
static void test_sx127x_no_frame_is_a_normal_probe(void)
{
    static const struct {
        const char *name;
        uint8_t stat;
    } cases[] = {
        { "signal detected alone", 0x01 },
        { "modem clear", 0x10 },
    };

    for (const auto &c : cases) {
        Sx127xRig rig;
        const std::string n = std::string("sx127x ") + c.name + ": ";

        rig.model.poke(REG_MODEM_STAT, c.stat);
        RadioCadProbeResult r = radio_cad_probe(&rig.ctrl);
        expect_int(n + "scanned", r.scan_ran, 1);
        expect_int(n + "FREE", r.status, RADIO_CAD_PROBE_FREE);
        expect_int(n + "entered CAD", rig.model.cad_entries, 1);
    }
}

/* A latched RxDone wins over the modem status: the completed frame takes the
 * pending-packet path (standby, BUSY, received set for the drain). */
static void test_sx127x_rx_done_is_not_in_progress(void)
{
    Sx127xRig rig;

    rig.model.poke(REG_MODEM_STAT, 0x0B);
    rig.model.poke(Sx127xRegisterModel::REG_IRQ_FLAGS, RADIOLIB_SX127X_CLEAR_IRQ_FLAG_RX_DONE);
    RadioCadProbeResult r = radio_cad_probe(&rig.ctrl);
    expect_int("sx127x RxDone: BUSY", r.status, RADIO_CAD_PROBE_BUSY);
    expect_int("sx127x RxDone: quiesced first", rig.model.standby_entries, 1);
    expect_int("sx127x RxDone: received set for the drain",
               rig.ctrl.received.load() ? 1 : 0, 1);
    expect_int("sx127x RxDone: no CAD", rig.model.cad_entries, 0);
}

/* FSK: the probe never reaches the chip, as before. */
static void test_sx127x_fsk_unchanged(void)
{
    Sx127xRig rig;

    rig.ctrl.mode = RADIO_MODE_FSK;
    rig.model.poke(REG_MODEM_STAT, 0x0B);
    RadioCadProbeResult r = radio_cad_probe(&rig.ctrl);
    expect_int("sx127x FSK: UNAVAILABLE", r.status, RADIO_CAD_PROBE_UNAVAILABLE);
    expect_int("sx127x FSK: no standby", rig.model.standby_entries, 0);
    expect_int("sx127x FSK: no CAD", rig.model.cad_entries, 0);
}

int main(void)
{
    test_sx1262_rx_latches_the_preamble();
    test_sx1262_frame_in_progress_is_left_alone();
    test_sx1262_ended_frame_is_not_in_progress();
    test_sx1262_fsk_rx_is_radiolib_default();
    test_sx1262_fsk_unchanged();
    test_sx127x_frame_in_progress_is_left_alone();
    test_sx127x_no_frame_is_a_normal_probe();
    test_sx127x_rx_done_is_not_in_progress();
    test_sx127x_fsk_unchanged();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
