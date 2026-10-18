#ifndef LORAHAM_RADIO_HEALTH_H
#define LORAHAM_RADIO_HEALTH_H

/* --- Radio health state --- */

typedef enum {
    RADIO_HEALTH_UNINITIALIZED = 0,
    RADIO_HEALTH_READY,
    RADIO_HEALTH_FAILED,
    /* Meshstick only: the USB device was lost after READY (being classified,
     * or waiting for the stick to return). Not ready. */
    RADIO_HEALTH_UNPLUGGED
} RadioHealth;

const char *radio_health_name(RadioHealth health);
int radio_health_is_ready(RadioHealth health);

#endif
