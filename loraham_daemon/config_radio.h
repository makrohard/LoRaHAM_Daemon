#ifndef LORAHAM_CONFIG_RADIO_H
#define LORAHAM_CONFIG_RADIO_H

#include <stddef.h>

#include "radio_channel.h"     /* RadioMode_t */
#include "radio_health.h"
#include "radio_rf_defaults.h"

/* --- GET RADIO (1.5.0): the radio parameters in effect ---------------------- */
/*
 * Formatted from memory on the main loop, like GET STATUS: the replay record
 * (the last successful SET per key since boot or the last MODE switch,
 * config_replay.h), else the default the daemon applied itself (the band
 * table in LoRa mode; the band FREQ and POWER in FSK). No chip read and no
 * radio lock, so a query never delays RX or TX. One snapshot: the caller
 * loads the health once and passes it in.
 *
 *   RADIO RADIO=<health> MODE=LORA|FSK FREQ= SF= BW= CR= PREAMBLE= SYNC= CRC= LDRO= POWER=\n
 *
 * FREQ, BW: decimal, one to six places (433.9, 125.0, 433.175); SF, CR,
 * PREAMBLE, POWER: integers; SYNC: 0x%02X; CRC: 0/1; LDRO: AUTO/0/1.
 * "-": the key does not apply in this MODE (the LoRa keys in FSK) or is not
 * reported (FSK PREAMBLE/SYNC before a successful SET; no band table).
 * "?": a recorded value that does not parse. The values are what was applied,
 * qualified by RADIO=: they describe the chip only with RADIO=READY.
 */
#define CONFIG_RADIO_LINE_MAX 256

void config_radio_format(char *buf, size_t buf_size, RadioHealth health, RadioMode_t mode,
                         const RadioRfDefaults *defaults);

/* One applied value from the replay record, normalized to the SET grammar
 * (also STATUS FREQ=, SF=, POWER=): a decimal with one to six places, or a
 * plain integer; "-" when the key is not in the record, "?" if its text does
 * not parse. Never empty, never a control character. */
void config_radio_applied(const char *key, bool decimal, char *out, size_t out_size);

#endif
