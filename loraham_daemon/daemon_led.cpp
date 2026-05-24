#include "daemon_led.h"

#include <stdio.h>
#include <unistd.h>

#include <lgpio.h>

/* --- LED/GPIO helpers ---------------------------------------------------- */

static int daemon_led_chip = -1;

void daemon_led_init(void)
{
    daemon_led_chip = lgGpiochipOpen(0);

    if (daemon_led_chip < 0) {
        printf("[GPIO] Fehler: gpiochip0 konnte nicht geöffnet werden!\n");
        return;
    }
}

int daemon_led_ready(void)
{
    return daemon_led_chip >= 0 ? 1 : 0;
}

void daemon_led_set_pin(int pin, int state)
{
    if (daemon_led_ready())
        lgGpioWrite(daemon_led_chip, pin, state ? 1 : 0);
}

void daemon_led_flash_pin(int pin)
{
    daemon_led_set_pin(pin, 1);
    usleep(15000);
    daemon_led_set_pin(pin, 0);
    usleep(15000);
}
