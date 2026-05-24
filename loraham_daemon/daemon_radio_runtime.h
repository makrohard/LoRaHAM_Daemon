#ifndef LORAHAM_DAEMON_RADIO_RUNTIME_H
#define LORAHAM_DAEMON_RADIO_RUNTIME_H

#include "daemon_led.h"
#include "radio_controller.h"

/* --- Radio runtime helpers ---------------------------------------------- */

extern RadioController<SX1278> radio_controller_433;
extern RadioController<RFM95> radio_controller_868;

void daemon_radio_controller_init(void);
void daemon_radio_shutdown_cleanup(void);
bool daemon_selected_radio_ready(void);
void daemon_log_active_radios(void);

void setFlag433(void);
void setFlag868(void);
void setFlashFlag433(void);
void setFlashFlag868(void);

template<typename RadioT>
static inline void daemon_radio_runtime_led(RadioController<RadioT> *ctrl,
                                            int state)
{
    if (!ctrl)
        return;

    daemon_led_set_pin(ctrl->led_pin, state);
}

template<typename RadioT>
static inline void daemon_radio_runtime_flash_led(RadioController<RadioT> *ctrl)
{
    if (!ctrl)
        return;

    daemon_led_flash_pin(ctrl->led_pin);
}

#endif
