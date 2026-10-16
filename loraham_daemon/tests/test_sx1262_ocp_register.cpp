/*
 * SX1262 over-current limit: written after every RadioLib init.
 *
 * RadioLib sets RegOcp (0x08E7) to 60 mA inside begin() and beginFSK(), and
 * setOutputPower() reads it and writes the same value back, so a driver that
 * never sets it runs every power level on 60 mA. The daemon writes --ocp, or
 * 140 mA without it, after both init paths. These tests run the REAL Sx1262Driver against the
 * REAL pinned RadioLib, with tests/fakes/sx126x_register_model.h where the
 * chip would be, and read the register back after boot, after a SET POWER,
 * after MODE=FSK and after the return to LoRa.
 *
 * The model answers only the commands it has been checked against. The last
 * test fails on any opcode outside that list, so a RadioLib change that
 * sends something new on these paths is noticed instead of answered blindly.
 */

#include "../sx1262_driver.h"
#include "fakes/sx126x_register_model.h"

#include <stdio.h>

/* ---- assert helpers ------------------------------------------------------ */

static int g_ok = 0;
static int g_fail = 0;

static void expect_hex(const char *name, unsigned got, unsigned want)
{
    if (got == want) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected 0x%02X, got 0x%02X\n", name, want, got);
    }
}

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

/* ---- the chip ------------------------------------------------------------ */

static const uint16_t REG_OCP = 0x08E7;
static const uint8_t OCP_60MA  = 60 / 2.5;    /* 0x18, what RadioLib leaves */
static const uint8_t OCP_100MA = 100 / 2.5;   /* 0x28 */
static const uint8_t OCP_140MA = 140 / 2.5;   /* 0x38 */

/* ---- rig ----------------------------------------------------------------- */

struct Rig {
    Sx126xRegisterModel model;
    Module mod;
    Sx1262Driver drv;

    /* waveshare-sx1262 pins and TCXO; no TXEN line, so RX/TX switching is
     * DIO2 only and no GPIO is involved. */
    explicit Rig(int ocp_ma = 140)
      : mod(&model, 21, 16, 18, 20), drv(&mod, 1.8f, -1, ocp_ma) { model.reset(); }
};

static RadioRfDefaults lora_defaults(void)
{
    RadioRfDefaults def{};

    def.freq_mhz = 869.525f;
    def.spreading_factor = 11;
    def.bandwidth_khz = 250.0f;
    def.sync_word = 0x2B;
    def.preamble_len = 16;
    def.coding_rate = 5;
    def.crc_on = true;
    def.ldro = -1;
    def.power_dbm = 10;

    return def;
}

/* ---- tests --------------------------------------------------------------- */

static void test_boot_and_mode_switch_leave_140(void)
{
    Rig rig;
    RadioRfDefaults def = lora_defaults();

    expect_int("boot succeeds", rig.drv.begin(&def), RADIOLIB_ERR_NONE);
    expect_hex("boot: OCP 140 mA", rig.model.peek(REG_OCP), OCP_140MA);

    expect_int("SET POWER=14 accepted",
               rig.drv.applyLoraParam("t", "POWER", "14"), RADIOLIB_ERR_NONE);
    printf("\n");
    expect_hex("SET POWER: OCP still 140 mA", rig.model.peek(REG_OCP), OCP_140MA);

    expect_int("MODE=FSK succeeds",
               rig.drv.switchMode(RADIO_MODE_FSK, &def), RADIOLIB_ERR_NONE);
    expect_hex("FSK: OCP 140 mA", rig.model.peek(REG_OCP), OCP_140MA);

    expect_int("MODE=LORA succeeds",
               rig.drv.switchMode(RADIO_MODE_LORA, &def), RADIOLIB_ERR_NONE);
    expect_hex("back to LoRa: OCP 140 mA", rig.model.peek(REG_OCP), OCP_140MA);
}

/* --ocp 100: the given value on the same paths: boot, SET POWER, FSK, back. */
static void test_ocp_100_on_every_path(void)
{
    Rig rig(100);
    RadioRfDefaults def = lora_defaults();

    expect_int("--ocp 100: boot succeeds", rig.drv.begin(&def), RADIOLIB_ERR_NONE);
    expect_hex("--ocp 100: boot", rig.model.peek(REG_OCP), OCP_100MA);

    expect_int("--ocp 100: SET POWER=14 accepted",
               rig.drv.applyLoraParam("t", "POWER", "14"), RADIOLIB_ERR_NONE);
    printf("\n");
    expect_hex("--ocp 100: SET POWER", rig.model.peek(REG_OCP), OCP_100MA);

    expect_int("--ocp 100: MODE=FSK succeeds",
               rig.drv.switchMode(RADIO_MODE_FSK, &def), RADIOLIB_ERR_NONE);
    expect_hex("--ocp 100: FSK", rig.model.peek(REG_OCP), OCP_100MA);

    expect_int("--ocp 100: MODE=LORA succeeds",
               rig.drv.switchMode(RADIO_MODE_LORA, &def), RADIOLIB_ERR_NONE);
    expect_hex("--ocp 100: back to LoRa", rig.model.peek(REG_OCP), OCP_100MA);
}

/* The driver returns setCurrentLimit()'s result. With RadioLib 187ef247 that
 * fails only on an out-of-range value: its writeRegister() returns ERR_NONE
 * whatever the chip answers, so no SPI-level fault can make the write fail.
 * 145 mA is outside RadioLib's 0..140 and is refused by --ocp's validation;
 * the driver is constructed with it directly, past that validation, and
 * begin() must return RadioLib's error instead of carrying on. The FSK path
 * calls the same sx1262_apply_ocp() after beginFSK() and cannot fail on its
 * own: a value that fails there has already failed begin(). */
static void test_out_of_range_ocp_fails_begin(void)
{
    Rig rig(145);
    RadioRfDefaults def = lora_defaults();

    expect_int("OCP 145 mA (past validation): begin() returns RadioLib's error",
               rig.drv.begin(&def), RADIOLIB_ERR_INVALID_CURRENT_LIMIT);
}

/* Negative control: the model itself reproduces what RadioLib does on its own,
 * so a pass above is the driver's doing and not the model's. */
static void test_radiolib_alone_leaves_60(void)
{
    Sx126xRegisterModel model;
    Module mod(&model, 21, 16, 18, 20);
    SX1262 radio(&mod);

    expect_int("RadioLib begin() succeeds on the model",
               radio.begin(869.525f, 250.0f, 11, 5, 0x2B, 10, 16, 1.8f, false),
               RADIOLIB_ERR_NONE);
    expect_hex("RadioLib alone: OCP 60 mA", model.peek(REG_OCP), OCP_60MA);
}

/* Every opcode the paths above sent, each one checked against what the model
 * answers. Anything new fails here. */
static void test_only_checked_opcodes_are_sent(void)
{
    /* Modelled: the four with state. Read, answered zero: GetDeviceErrors
     * (setTCXO checks XOSC_START_ERR; zero = none) and GetStatus (only the
     * status byte is read). The rest are writes whose only reply RadioLib
     * reads is the status byte, which the model answers with STATUS_OK. */
    static const struct { uint8_t op; const char *name; } checked[] = {
        { 0x0D, "WriteRegister (stored)" },
        { 0x1D, "ReadRegister (answered from the map)" },
        { 0x8A, "SetPacketType (stored)" },
        { 0x11, "GetPacketType (answered)" },
        { 0x17, "GetDeviceErrors (read, zero = no error)" },
        { 0xC0, "GetStatus (status byte only)" },
        { 0x00, "NOP (standby wake-up, reply ignored)" },
        { 0x02, "ClearIrqStatus (write)" },
        { 0x08, "SetDioIrqParams (write)" },
        { 0x80, "SetStandby (write)" },
        { 0x86, "SetRfFrequency (write)" },
        { 0x88, "SetCadParams (write)" },
        { 0x89, "Calibrate (write)" },
        { 0x8B, "SetModulationParams (write)" },
        { 0x8C, "SetPacketParams (write)" },
        { 0x8E, "SetTxParams (write)" },
        { 0x8F, "SetBufferBaseAddress (write)" },
        { 0x93, "SetRxTxFallbackMode (write)" },
        { 0x95, "SetPaConfig (write)" },
        { 0x96, "SetRegulatorMode (write)" },
        { 0x97, "SetDio3AsTcxoCtrl (write)" },
        { 0x98, "CalibrateImage (write)" },
        { 0x9D, "SetDio2AsRfSwitchCtrl (write)" },
    };

    Rig rig;
    RadioRfDefaults def = lora_defaults();
    rig.drv.begin(&def);
    rig.drv.applyLoraParam("t", "POWER", "14");
    printf("\n");
    rig.drv.switchMode(RADIO_MODE_FSK, &def);
    rig.drv.switchMode(RADIO_MODE_LORA, &def);

    int unchecked = 0;
    for (int op = 0; op < 256; op++) {
        if (!rig.model.seen[op])
            continue;
        const char *name = nullptr;
        for (const auto &c : checked)
            if (c.op == op)
                name = c.name;
        printf("[TRACE] opcode 0x%02X %s\n", op, name ? name : "NOT CHECKED");
        if (!name)
            unchecked++;
    }
    expect_int("no opcode outside the checked list", unchecked, 0);
}

int main(void)
{
    test_boot_and_mode_switch_leave_140();
    test_ocp_100_on_every_path();
    test_out_of_range_ocp_fails_begin();
    test_radiolib_alone_leaves_60();
    test_only_checked_opcodes_are_sent();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
