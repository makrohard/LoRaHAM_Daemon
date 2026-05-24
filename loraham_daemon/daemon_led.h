#ifndef LORAHAM_DAEMON_LED_H
#define LORAHAM_DAEMON_LED_H

/* --- LED/GPIO helpers ---------------------------------------------------- */

#define DAEMON_LED_PIN_433 13
#define DAEMON_LED_PIN_868 19

void daemon_led_init(void);
int daemon_led_ready(void);
void daemon_led_set_pin(int pin, int state);
void daemon_led_flash_pin(int pin);

#endif
