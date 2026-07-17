#include "daemon_band.h"

#include <stdio.h>
#include <stdlib.h>

#include "daemon_led.h"
#include "daemon_protocol.h"
#include "radio_rf_defaults.h"

/* --- Boot-RF-Defaults ------------------------------------------------------ */
// Per-band boot defaults, applied inside RadioDriver::begin() in the exact
// pre-driver setter order. LDRO: 433 = autoLDRO()+forceLDRO(1), 868 = nur
// autoLDRO() (Feld ldro: >=0 forciert, <0 auto).

// LoRa-APRS:
static const RadioRfDefaults rf_defaults_433 = {
    433.900f,   /* freq_mhz */
    12,         /* spreading_factor */
    125.0f,     /* bandwidth_khz */
    0x12,       /* sync_word */
    8,          /* preamble_len */
    5,          /* coding_rate */
    true,       /* crc_on */
    1,          /* ldro: autoLDRO() + forceLDRO(1) */
    10          /* power_dbm */
};

/*
*
* // LoRa DX Cluster:
*    433.900 / SF10 / BW125.0 / Sync 0x12 / Preamble 8 / CR5 / CRC an /
*    autoLDRO() / 10 dBm
*
*
*
* // Meshtastic 433:
*
*    433.900 (DB0ARD) / SF11 / BW125.0 / Sync 0x2B / Preamble 16 / CR5 /
*    CRC an / autoLDRO() / 10 dBm
*
*
* // Meshcom:
*
*    433.175 / SF11 / BW250.0 / Sync 0x2B / Preamble 8 / CR6 / CRC an /
*    (kein autoLDRO) / 10 dBm
*/

static const RadioRfDefaults rf_defaults_868 = {
    869.525f,   /* freq_mhz */
    11,         /* spreading_factor */
    250.0f,     /* bandwidth_khz */
    0x2B,       /* sync_word */
    16,         /* preamble_len */
    5,          /* coding_rate */
    true,       /* crc_on */
    -1,         /* ldro: nur autoLDRO() */
    10          /* power_dbm */
};

/* --- Band descriptors ------------------------------------------------------ */

static const DaemonBandDescriptor band_433 = {
    RADIO_BAND_433,
    433,
    "433",
    false,
    DATA433_SOCKET,
    DATA433_FRAMED_SOCKET,
    CONF433_SOCKET,
    "TX433",
    "TX433F",
    "RX433",
    "CONF433",
    "CONFIG433",
    "CLIENT433",
    "CAD433",
    "RSSI433",
    430.0f,
    440.0f,
    &rf_defaults_433,
    DAEMON_LED_PIN_433
};

static const DaemonBandDescriptor band_868 = {
    RADIO_BAND_868,
    868,
    "868",
    true,
    DATA868_SOCKET,
    DATA868_FRAMED_SOCKET,
    CONF868_SOCKET,
    "TX868",
    "TX868F",
    "RX868",
    "CONF868",
    "CONFIG868",
    "CLIENT868",
    "CAD868",
    "RSSI868",
    863.0f,
    870.0f,
    &rf_defaults_868,
    DAEMON_LED_PIN_868
};

static const DaemonBandDescriptor *g_band = NULL;

void daemon_band_resolve(RadioBand_t band)
{
    g_band = (band == RADIO_BAND_868) ? &band_868 : &band_433;
}

const DaemonBandDescriptor *daemon_band(void)
{
    if (!g_band) {
        /* Fail closed: reading the band before --radio resolution is a
         * programming error, never a recoverable runtime state. */
        fprintf(stderr, "[Daemon] daemon_band() vor Auflösung gelesen\n");
        abort();
    }

    return g_band;
}
