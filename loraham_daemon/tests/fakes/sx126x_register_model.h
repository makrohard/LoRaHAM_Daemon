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
 *   0x08 SetDioIrqParams [op, 8 mask bytes]                stored; copied to cad_dio_irq
 *                                                          by the next SetCadParams
 *   0x88 SetCadParams   [op, 7 parameter bytes]            stored (cad_params)
 *   0x12 GetIrqStatus   [op, status, irqHi, irqLo]         answered from irq_status
 *   anything else       accepted; reply bytes after the status are zero
 *
 * The parameter bytes of the last command per opcode are kept (op_bytes), and
 * the last attachInterrupt/detachInterrupt, for the register-image test.
 *
 * Every opcode is recorded, so the test can name exactly which commands
 * begin() and beginFSK() send and fail on one the model has not been checked
 * against. Zero is the right answer for the one command on these paths that
 * reads data back besides the four above (GetDeviceErrors): no error.
 *
 * BUSY reads LOW at all times, so RadioLib's BUSY waits end at once. The IRQ
 * pin reads LOW too unless a test names it in irq_high_pin, so RadioLib's
 * unbounded CAD wait (scanChannel) can end; irq_status is 0 unless set.
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
    static const uint8_t OP_SET_CAD_PARAMS = 0x88;
    static const uint8_t OP_SET_DIO_IRQ_PARAMS = 0x08;
    static const uint8_t OP_GET_IRQ_STATUS = 0x12;

    static const uint16_t REG_VERSION_STRING = 0x0320;

    /* STBY_RC, no command error: anything but 0x00/0xFF and the three error
     * codes in bits 3:1 is accepted by SX126x::SPIparseStatus(). */
    static const uint8_t STATUS_OK = 0x22;

    /* --- observations ---------------------------------------------------- */
    bool seen[256];
    uint8_t cad_params[7];        /* the last SetCadParams bytes */
    uint8_t dio_irq[8];           /* the last SetDioIrqParams bytes */
    uint8_t cad_dio_irq[8];       /* the SetDioIrqParams in force at the last SetCadParams */
    int cad_params_count = 0;     /* SetCadParams commands since reset() */
    int irq_high_pin = -1;        /* the pin that reads HIGH; -1 = none */
    uint16_t irq_status = 0;      /* what GetIrqStatus answers */
    /* The parameter bytes (after the opcode) of the last command per opcode,
     * capped at OP_BYTES_MAX, and their count. */
    static const size_t OP_BYTES_MAX = 24;
    uint8_t op_bytes[256][OP_BYTES_MAX];
    uint8_t op_len[256];
    /* The last attachInterrupt (pin, mode) and detachInterrupt pin; -1 = none. */
    int attach_pin = -1;
    uint32_t attach_mode = 0;
    int detach_pin = -1;

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
        memset(cad_params, 0, sizeof(cad_params));
        memset(dio_irq, 0, sizeof(dio_irq));
        memset(cad_dio_irq, 0, sizeof(cad_dio_irq));
        cad_params_count = 0;
        memset(op_bytes, 0, sizeof(op_bytes));
        memset(op_len, 0, sizeof(op_len));
        attach_pin = -1;
        attach_mode = 0;
        detach_pin = -1;
        /* findChip() compares the first six bytes with "SX1261" -- the
         * SX1262 reports the SX1261 string, and RadioLib expects that. */
        memcpy(&regs_[REG_VERSION_STRING], "SX1261 V2D 2D02", 16);
        packet_type_ = 0;
    }

    /* Forget which opcodes were sent and their bytes (the register map
     * stays): the next image shows only what the following path sends. */
    void forget_ops()
    {
        memset(seen, 0, sizeof(seen));
        memset(op_bytes, 0, sizeof(op_bytes));
        memset(op_len, 0, sizeof(op_len));
    }

    /* What GetPacketType answers next (RadioLib refuses LoRa setters off LoRa). */
    void set_packet_type(uint8_t type) { packet_type_ = type; }

    uint8_t peek(uint16_t addr) const { return regs_[addr]; }
    void poke(uint16_t addr, uint8_t value) { regs_[addr] = value; }

    /* --- RadioLibHal ----------------------------------------------------- */
    void pinMode(uint32_t, uint32_t) override { }
    void digitalWrite(uint32_t, uint32_t) override { }
    uint32_t digitalRead(uint32_t pin) override   /* BUSY low */
    {
        return irq_high_pin >= 0 && pin == (uint32_t)irq_high_pin ? 1 : 0;
    }
    void attachInterrupt(uint32_t pin, void (*)(void), uint32_t mode) override
    {
        attach_pin = (int)pin;
        attach_mode = mode;
    }
    void detachInterrupt(uint32_t pin) override { detach_pin = (int)pin; }
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
        op_len[op] = (uint8_t)(len - 1 < OP_BYTES_MAX ? len - 1 : OP_BYTES_MAX);
        memcpy(op_bytes[op], out + 1, op_len[op]);

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
        } else if (op == OP_SET_DIO_IRQ_PARAMS && len >= 9) {
            memcpy(dio_irq, out + 1, sizeof(dio_irq));
        } else if (op == OP_SET_CAD_PARAMS && len >= 8) {
            memcpy(cad_params, out + 1, sizeof(cad_params));
            memcpy(cad_dio_irq, dio_irq, sizeof(cad_dio_irq));
            cad_params_count++;
        } else if (op == OP_GET_IRQ_STATUS && len > 3 && in) {
            in[2] = (uint8_t)(irq_status >> 8);
            in[3] = (uint8_t)(irq_status & 0xFF);
        }
    }

  private:
    uint8_t regs_[0x10000];
    uint8_t packet_type_ = 0;
    RadioLibTime_t clock_us_ = 0;
};

#endif
