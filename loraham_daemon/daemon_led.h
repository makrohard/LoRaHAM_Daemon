#ifndef LORAHAM_DAEMON_LED_H
#define LORAHAM_DAEMON_LED_H

/* --- LED/GPIO helpers ---------------------------------------------------- */

/* Default (legacy-profile) LED lines; the hardware profile may override them
 * via daemon_led_configure(), including < 0 = no LED (feature disabled). */
#define DAEMON_LED_PIN_433 13
#define DAEMON_LED_PIN_868 19

/* Override the per-band LED line before daemon_led_init(); < 0 disables the
 * LED for that band (no claim, writes become no-ops, init stays healthy). */
void daemon_led_configure(int pin_433, int pin_868);
int daemon_led_pin_433_configured(void);
int daemon_led_pin_868_configured(void);

int daemon_led_init(void);
void daemon_led_shutdown(void);
int daemon_led_ready(void);
void daemon_led_set_pin(int pin, int state);

#endif
