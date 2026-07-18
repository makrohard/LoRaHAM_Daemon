#ifndef LORAHAM_HARDWARE_PROFILE_H
#define LORAHAM_HARDWARE_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- Hardware profiles ---------------------------------------------------- */
/*
 * A HardwareProfile describes wiring and silicon for THE one radio of a
 * process (one daemon process per band). Presets are selected with the flat
 * CLI flag `--hw <preset>` (default: "legacy", which resolves per band to the
 * wiring hardcoded before profiles existed).
 *
 * Pin semantics are family-dependent (RadioLib Module(hal, cs, irq, rst, gpio)):
 *   SX127x: irq = DIO0, gpio = DIO1
 *   SX1262: irq = DIO1, gpio = BUSY
 * A pin value < 0 means "not connected" and is passed as RADIOLIB_NC.
 *
 * Adding a future board of a known chip family == adding one preset row in
 * hardware_profile.cpp plus README docs; nothing else (see README, section
 * "Adding new hardware").
 */

#define DAEMON_HW_PIN_NC (-1)

typedef enum {
    DAEMON_CHIP_FAMILY_SX127X = 0,   /* SX127x/RFM9x lineage */
    DAEMON_CHIP_FAMILY_SX1262        /* SX126x lineage */
} DaemonChipFamily;

#define DAEMON_HW_MAX_CLAIMED 8

typedef struct {
    const char *name;          /* preset name as given to --hw */
    DaemonChipFamily family;

    /* RadioLib Module() arguments (family semantics, see above). */
    int cs;
    int irq;                   /* SX127x: DIO0; SX1262: DIO1 */
    int rst;                   /* < 0: reset not wired (warm start) */
    int gpio;                  /* SX127x: DIO1; SX1262: BUSY */

    /* Documentation-only / auxiliary lines (not passed to Module()). */
    int aux;                   /* SX127x DIO5 where routed; doc only */
    int txen;                  /* SX1262 TXEN/ANT_SW; < 0: none */
    int rxen;                  /* < 0: none (SX1262: RX switching via DIO2) */

    float tcxo_voltage;        /* > 0: begin() must set DIO3 TCXO voltage */

    int led_pin;               /* < 0: no status LED for this process */

    /* Capabilities. */
    bool cad_scan_available;   /* true: scanChannel()-based CAD usable
                                * (SX127x needs DIO1 wired: without it the
                                * blocking scanChannel can never see
                                * CadDetected and would report false-FREE). */
    bool fsk_stream_available; /* SX127x FSK stream modes need DIO1. */
    bool reset_wired;          /* false: log warm-start note once at init */

    /* All BCM lines this profile claims (LED + Module pins); for claim-failure
     * diagnostics and docs. */
    int claimed[DAEMON_HW_MAX_CLAIMED];
    int claimed_count;
} DaemonHardwareProfile;

/* Selected preset name (CLI); resolved profile for this process. */
extern DaemonHardwareProfile daemon_hw_profile;

/*
 * Record the --hw preset name. Returns false for NULL. Validation happens in
 * daemon_hardware_profile_resolve() once the band is known.
 */
bool daemon_set_hardware_preset(const char *name);
const char *daemon_hardware_preset_name(void);

/*
 * Resolve the selected preset for the given band (433 or 868) into
 * daemon_hw_profile. Returns false for an unknown preset name.
 * "legacy" resolves per band exactly to the pre-profile hardcoded wiring.
 */
bool daemon_hardware_profile_resolve(int band);

/* Human-readable list of preset names for usage/error text. */
const char *daemon_hardware_profile_known(void);

const char *daemon_chip_family_name(DaemonChipFamily family);

#ifdef __cplusplus
}
#endif

#endif
