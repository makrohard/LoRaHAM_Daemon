#ifndef LORAHAM_TEST_FAKE_SX126X_REGISTER_MODEL_H
#define LORAHAM_TEST_FAKE_SX126X_REGISTER_MODEL_H

/*
 * A minimal SX126x behind RadioLib's own HAL interface -- just enough for the
 * REAL Sx1262Driver and the REAL pinned RadioLib to run begin() and
 * switchMode(), so a test can read back the registers they leave behind.
 *
 * It is deliberately not a radio. The SX126x is command-driven: byte 0 of
 * every SPI transaction is an opcode, and RadioLib reads the chip status from
 * byte 1 of the reply (spiConfig.statusPos). What the model does per opcode:
 *
 *   0x0D WriteRegister  [op, addrHi, addrLo, data...]      stored
 *   0x1D ReadRegister   [op, addrHi, addrLo, NOP, data...] answered from the map
 *   0x8A SetPacketType  [op, type]                         stored
 *   0x11 GetPacketType  [op, status, type]                 answered
 *   anything else       accepted; reply bytes after the status are zero
 *
 * Every opcode is recorded, so the test can name exactly which commands
 * begin() and beginFSK() send and fail on one the model has not been checked
 * against. Zero is the right answer for the one command on these paths that
 * reads data back besides the four above (GetDeviceErrors): no error.
 *
 * BUSY reads LOW at all times, so RadioLib's BUSY waits end at once.
 */

#include <stdint.h>
#include <string.h>

#include <RadioLib.h>

class Sx126xRegisterModel : public RadioLibHal {
  public:
    static const uint8_t OP_WRITE_REGISTER = 0x0D;
    static const uint8_t OP_READ_REGISTER  = 0x1D;
    static const uint8_t OP_SET_PACKET_TYPE = 0x8A;
    static const uint8_t OP_GET_PACKET_TYPE = 0x11;

    static const uint16_t REG_VERSION_STRING = 0x0320;

    /* STBY_RC, no command error: anything but 0x00/0xFF and the three error
     * codes in bits 3:1 is accepted by SX126x::SPIparseStatus(). */
    static const uint8_t STATUS_OK = 0x22;

    /* --- observations ---------------------------------------------------- */
    bool seen[256];

    Sx126xRegisterModel()
      : RadioLibHal(/*input*/ 0, /*output*/ 1, /*low*/ 0, /*high*/ 1,
                    /*rising*/ 1, /*falling*/ 2)
    {
        reset();
    }

    void reset()
    {
        memset(regs_, 0, sizeof(regs_));
        memset(seen, 0, sizeof(seen));
        /* findChip() compares the first six bytes with "SX1261" -- the
         * SX1262 reports the SX1261 string, and RadioLib expects that. */
        memcpy(&regs_[REG_VERSION_STRING], "SX1261 V2D 2D02", 16);
        packet_type_ = 0;
    }

    uint8_t peek(uint16_t addr) const { return regs_[addr]; }
    void poke(uint16_t addr, uint8_t value) { regs_[addr] = value; }

    /* --- RadioLibHal ----------------------------------------------------- */
    void pinMode(uint32_t, uint32_t) override { }
    void digitalWrite(uint32_t, uint32_t) override { }
    uint32_t digitalRead(uint32_t) override { return 0; }   /* BUSY low */
    void attachInterrupt(uint32_t, void (*)(void), uint32_t) override { }
    void detachInterrupt(uint32_t) override { }
    void delay(RadioLibTime_t) override { }
    void delayMicroseconds(RadioLibTime_t) override { }
    /* A clock that moves when read, so a bounded RadioLib wait cannot spin. */
    RadioLibTime_t millis() override { clock_us_ += 1000; return clock_us_ / 1000; }
    RadioLibTime_t micros() override { clock_us_ += 100; return clock_us_; }
    long pulseIn(uint32_t, uint32_t, RadioLibTime_t) override { return 0; }

    void spiBegin() override { }
    void spiBeginTransaction() override { }
    void spiEndTransaction() override { }
    void spiEnd() override { }

    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override
    {
        if (len == 0)
            return;

        const uint8_t op = out[0];
        seen[op] = true;

        if (in) {
            memset(in, 0, len);
            if (len > 1)
                in[1] = STATUS_OK;
        }

        if (op == OP_WRITE_REGISTER && len > 3) {
            uint16_t addr = (uint16_t)((out[1] << 8) | out[2]);
            for (size_t i = 3; i < len; i++)
                regs_[addr++] = out[i];
        } else if (op == OP_READ_REGISTER && len > 4 && in) {
            uint16_t addr = (uint16_t)((out[1] << 8) | out[2]);
            for (size_t i = 4; i < len; i++)
                in[i] = regs_[addr++];
        } else if (op == OP_SET_PACKET_TYPE && len > 1) {
            packet_type_ = out[1];
        } else if (op == OP_GET_PACKET_TYPE && len > 2 && in) {
            in[2] = packet_type_;
        }
    }

  private:
    uint8_t regs_[0x10000];
    uint8_t packet_type_ = 0;
    RadioLibTime_t clock_us_ = 0;
};

#endif
