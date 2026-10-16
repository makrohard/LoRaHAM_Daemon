/*
 * Ch341Hal: the Meshstick's RadioLib HAL, and the daemon contract it keeps.
 *
 * The REAL Ch341Hal runs the REAL Sx1262Driver and the REAL pinned RadioLib,
 * with tests/fakes/ch341_fake_adapter.h where the adapter table would be and
 * the SX126x register model behind it. Pins are the Meshstick's CH341 lines:
 * CS D0, DIO1 D6, RESET D2, BUSY D4, RXEN D1.
 *
 * What it shows:
 *   - boot, SET POWER (22 dBm under the preset's ceiling), FSK and back work over this HAL, RegOcp is 140 mA, the
 *     RX interrupt lands on D6, RXEN (D1) is driven HIGH in RX and LOW in
 *     standby, and every SPI opcode sent is one the model has been checked
 *     against;
 *   - before the radio is operational a failed USB call latches a startup
 *     failure and does not exit; a device that never opened is "bus not
 *     ready";
 *   - the device is closed exactly once on every exit the daemon controls:
 *     the clean stop, a startup failure after open (begin() fails), and the
 *     exit 5 after a failed transfer or a lost device once operational.
 *     Closing is what re-attaches the kernel driver, and exit 5 is _exit(),
 *     which runs no destructor -- so those cases run in a forked child and
 *     the close is counted through the fake's close log.
 */

#include "../ch341_hal.h"
#include "../sx1262_driver.h"
#include "fakes/ch341_fake_adapter.h"
#include "../config_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/wait.h>

/* ---- assert helpers ------------------------------------------------------ */

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

/* ---- the stick ----------------------------------------------------------- */

static const uint16_t REG_OCP = 0x08E7;
static const uint8_t OCP_140MA = 140 / 2.5;   /* 0x38 */

static const uint32_t PIN_CS = 0, PIN_RXEN = 1, PIN_RST = 2, PIN_BUSY = 4, PIN_DIO1 = 6;

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

static void rx_cb(void) { }

/* ---- U4: the radio over the HAL ------------------------------------------ */

static void test_radio_over_ch341(void)
{
    Sx126xRegisterModel model;
    fake_ch341_reset(&model);
    {
        Ch341Hal hal(&g_fake_ch341_api, 0x1A86, 0x5512);
        Module mod(&hal, PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY);
        Sx1262Driver drv(&mod, 1.8f, (int)PIN_RXEN, 140);
        RadioRfDefaults def = lora_defaults();

        expect_int("device opened once", g_fake_ch341.opens, 1);
        expect_int("bus ready once open", hal.spi_lock_ready(), 1);

        expect_int("boot succeeds", drv.begin(&def), RADIOLIB_ERR_NONE);
        expect_hex("boot: OCP 140 mA", model.peek(REG_OCP), OCP_140MA);
        expect_int("CS (D0) driven, ends HIGH", g_fake_ch341.level[PIN_CS], 1);
        expect_int("RESET (D2) pulsed LOW then HIGH",
                   g_fake_ch341.writes_low[PIN_RST] > 0 && g_fake_ch341.level[PIN_RST] == 1, 1);

        drv.setPacketReceivedAction(rx_cb);
        expect_int("RX interrupt on D6", (long)g_fake_ch341.irq_pin, (long)PIN_DIO1);
        expect_int("RX interrupt callback installed", g_fake_ch341.irq_cb == rx_cb, 1);

        expect_int("startReceive succeeds", drv.startReceive(), RADIOLIB_ERR_NONE);
        expect_int("RX: RXEN (D1) HIGH", g_fake_ch341.level[PIN_RXEN], 1);
        expect_int("standby succeeds", drv.standby(), RADIOLIB_ERR_NONE);
        expect_int("standby: RXEN (D1) LOW", g_fake_ch341.level[PIN_RXEN], 0);

        expect_int("SET POWER=14 accepted",
                   drv.applyLoraParam("t", "POWER", "14"), RADIOLIB_ERR_NONE);
        printf("\n");
        config_policy_set_sx1262_power_max(22);   /* what meshstick-1262 sets at startup */
        expect_int("SET POWER=22 accepted under the 22 dBm ceiling",
                   drv.applyLoraParam("t", "POWER", "22"), RADIOLIB_ERR_NONE);
        printf("\n");
        config_policy_sx1262_power_max_reset();
        expect_int("MODE=FSK succeeds", drv.switchMode(RADIO_MODE_FSK, &def), RADIOLIB_ERR_NONE);
        expect_int("MODE=LORA succeeds", drv.switchMode(RADIO_MODE_LORA, &def), RADIOLIB_ERR_NONE);
        expect_hex("back to LoRa: OCP 140 mA", model.peek(REG_OCP), OCP_140MA);
        expect_int("no startup failure latched", hal.gpio_startup_ok(), 1);

        g_fake_ch341.read_raw[PIN_BUSY] = 16;   /* the library returns the masked bit */
        expect_int("a set input reads as 1, not the raw bit", (long)hal.digitalRead(PIN_BUSY), 1);
        g_fake_ch341.read_raw[PIN_BUSY] = 0;

        /* Every opcode sent, each one the model has been checked against
         * (the list of test_sx1262_ocp_register plus the RX/standby ones). */
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
            { 0x82, "SetRx (write)" },
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
        int unchecked = 0;
        for (int op = 0; op < 256; op++) {
            if (!model.seen[op])
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
        expect_int("not closed while in use", g_fake_ch341.closes, 0);
    }
    expect_int("clean stop: closed exactly once", g_fake_ch341.closes, 1);
}

/* ---- startup failures latch, they do not exit ------------------------------ */

static void test_startup_failures_latch(void)
{
    Sx126xRegisterModel model;

    /* The device never opened: bus not ready, nothing to close. */
    fake_ch341_reset(&model);
    g_fake_ch341.open_ok = false;
    {
        Ch341Hal hal(&g_fake_ch341_api, 0x1A86, 0x5512);
        uint8_t out[3] = { 0xC0, 0, 0 }, in[3] = { 9, 9, 9 };

        expect_int("not opened: bus not ready", hal.spi_lock_ready(), 0);
        hal.spiTransfer(out, sizeof(out), in);
        expect_int("not opened: transfer answers zeros", in[0] | in[1] | in[2], 0);
        expect_int("not opened: startup failure latched", hal.gpio_startup_ok(), 0);
    }
    expect_int("not opened: nothing closed", g_fake_ch341.closes, 0);

    /* Opened, a transfer fails before the radio is operational. */
    fake_ch341_reset(&model);
    {
        Ch341Hal hal(&g_fake_ch341_api, 0x1A86, 0x5512);
        uint8_t out[3] = { 0xC0, 0, 0 }, in[3] = { 9, 9, 9 };

        g_fake_ch341.fail_transceive = true;
        hal.spiTransfer(out, sizeof(out), in);
        expect_int("startup transfer failure: latched, process still here", hal.gpio_startup_ok(), 0);
        expect_int("startup transfer failure: zeros", in[0] | in[1] | in[2], 0);
        g_fake_ch341.in_error = true;
        hal.check_runtime();
        expect_int("before operational: a lost device does not exit", 1, 1);
    }
    expect_int("startup failure: closed once by the teardown", g_fake_ch341.closes, 1);

    /* Opened, begin() fails (no SX126x answers): the startup cleanup's
     * teardown closes the device. */
    fake_ch341_reset(&model);
    model.poke(0x0320, 'X');
    {
        Ch341Hal hal(&g_fake_ch341_api, 0x1A86, 0x5512);
        Module mod(&hal, PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY);
        Sx1262Driver drv(&mod, 1.8f, (int)PIN_RXEN, 140);
        RadioRfDefaults def = lora_defaults();

        expect_int("begin() fails on a chip that is not an SX126x",
                   drv.begin(&def) != RADIOLIB_ERR_NONE, 1);
    }
    expect_int("begin() failure after open: closed exactly once", g_fake_ch341.closes, 1);
}

/* ---- U7: exit 5 closes the device first -------------------------------- */

/* Run `inject` in a forked child on an operational HAL; return the child's
 * exit code and how many closes it logged. */
static void run_operational_child(const char *label, void (*inject)(Ch341Hal *hal))
{
    char log[] = "/tmp/loraham-ch341-close.XXXXXX";
    int fd = mkstemp(log);
    close(fd);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        Sx126xRegisterModel model;
        fake_ch341_reset(&model);
        g_fake_ch341.close_log = log;
        Ch341Hal *hal = new Ch341Hal(&g_fake_ch341_api, 0x1A86, 0x5512);
        hal->gpio_mark_operational();
        inject(hal);
        _exit(99);   /* not reached when the HAL exits 5 */
    }

    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    int closes = 0;
    FILE *f = fopen(log, "r");
    char line[16];
    while (f && fgets(line, sizeof(line), f))
        closes++;
    if (f)
        fclose(f);
    unlink(log);

    std::string t = std::string(label) + ": exit 5";
    expect_int(t.c_str(), code, LORAHAM_EXIT_RUNTIME_RADIO_IO_ERROR);
    t = std::string(label) + ": device closed exactly once before the exit";
    expect_int(t.c_str(), closes, 1);
}

static void inject_failed_transfer(Ch341Hal *hal)
{
    uint8_t out[3] = { 0xC0, 0, 0 }, in[3];
    g_fake_ch341.fail_transceive = true;
    hal->spiTransfer(out, sizeof(out), in);
}

static void inject_lost_device(Ch341Hal *hal)
{
    g_fake_ch341.in_error = true;
    hal->check_runtime();
}

int main(void)
{
    test_radio_over_ch341();
    test_startup_failures_latch();
    run_operational_child("operational, SPI transfer fails", inject_failed_transfer);
    run_operational_child("operational, device lost (poll thread)", inject_lost_device);

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
