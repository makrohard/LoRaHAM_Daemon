#ifndef LORAHAM_RADIO_CHANNEL_H
#define LORAHAM_RADIO_CHANNEL_H

/* --- Radio channel identity --- */

typedef enum {
    RADIO_BAND_433 = 433,
    RADIO_BAND_868 = 868
} RadioBand_t;

/* --- Radio modem mode --- */

typedef enum {
    RADIO_MODE_LORA,
    RADIO_MODE_FSK
} RadioMode_t;

#endif
