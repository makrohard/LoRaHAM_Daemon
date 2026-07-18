#ifndef LORAHAM_RADIO_DRIVER_H
#define LORAHAM_RADIO_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include <string>

#include <RadioLib.h>

#include "hardware_profile.h" /* DaemonChipFamily */
#include "radio_channel.h"   /* RadioMode_t */

/* --- Radio driver interface ----------------------------------------------- */
/*
 * Runtime interface between the chip-agnostic daemon runtime and one radio
 * chip family. Exactly ONE driver instance exists per process (one process
 * per band). Generic calls delegate to RadioLib PhysicalLayer virtuals
 * (verified against the deployment RadioLib tree); everything chip-specific
 * (RF-default begin(), LoRa/FSK parameter apply, mode switch, raw RSSI reads)
 * is pure-virtual and lives in the concrete driver.
 *
 * The generic delegates are virtual so unit-test fakes can intercept them
 * (counter semantics identical to the pre-driver template fakes).
 *
 * Adding a new chip family == implementing this interface; the daemon runtime
 * stays untouched (see README "Adding new hardware").
 */

#include "radio_rf_defaults.h"

class RadioDriver {
public:
    virtual ~RadioDriver() = default;

    /* --- Generic PhysicalLayer delegation (behavior-identical) --- */
    virtual int16_t transmit(const uint8_t *data, size_t len)
    {
        return phy_->transmit(data, len);
    }
    virtual int16_t startReceive() { return phy_->startReceive(); }
    virtual int16_t readData(uint8_t *data, size_t len)
    {
        return phy_->readData(data, len);
    }
    virtual size_t getPacketLength() { return phy_->getPacketLength(); }
    virtual float getRSSI() { return phy_->getRSSI(); }
    virtual float getSNR() { return phy_->getSNR(); }
    virtual int16_t standby() { return phy_->standby(); }
    virtual int16_t sleep() { return phy_->sleep(); }
    virtual int16_t clearIrq(uint32_t flags) { return phy_->clearIrq(flags); }
    virtual void setPacketReceivedAction(void (*func)(void))
    {
        phy_->setPacketReceivedAction(func);
    }
    virtual void clearPacketReceivedAction()
    {
        phy_->clearPacketReceivedAction();
    }
    virtual int16_t scanChannel() { return phy_->scanChannel(); }

    /* --- Chip-specific (pure virtual) --- */

    /* Initialize the chip and apply the boot RF defaults in the chip's
     * documented setter order. Returns the RadioLib begin() state. */
    virtual int16_t begin(const RadioRfDefaults *defaults) = 0;

    /* LoRa <-> FSK mode switch. Lands the radio on the BAND boot defaults
     * for the target mode (never chip defaults — an 868 process must not
     * park at 434 MHz). No default argument: every caller states its
     * defaults, fail-closed against reintroducing chip defaults. The caller
     * re-arms the RX callback and startReceive() afterwards, as before. */
    virtual int16_t switchMode(RadioMode_t mode,
                               const RadioRfDefaults *defaults) = 0;

    /* CONFIG SET parameter application for the current mode; prints the
     * per-key colored state exactly like the pre-driver implementation.
     * Returns the RadioLib state of the applied key (audit P1-2): 0 for
     * OK/no-op/value-rejected-before-hardware; a nonzero RadioLib error
     * means the chip rejected the operation and the apply must abort. */
    virtual int16_t applyLoraParam(const char *tag, const std::string &key,
                                   const std::string &val) = 0;
    virtual int16_t applyFskParam(const char *tag, const std::string &key,
                                  const std::string &val) = 0;

    /* Raw live channel RSSI (non-destructive; chip-native mechanism). */
    virtual float readLiveRssi(RadioMode_t mode, bool is_hf) = 0;

    /* Non-destructive instant-RSSI probe (SX127x: getRSSI(false, true)). */
    virtual float rssiProbe() = 0;

    virtual const char *chipName() const = 0;

    /* Chip family for family-specific CONF value rasters (e.g. FSK RXBW):
     * the transactional validation must accept exactly what the chip's
     * apply path will accept, per family. */
    virtual DaemonChipFamily chipFamily() const = 0;

protected:
    explicit RadioDriver(PhysicalLayer *phy) : phy_(phy) {}

    PhysicalLayer *phy_;
};

#endif
