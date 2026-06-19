#include "daemon_led.h"

#include <stdio.h>
#include <unistd.h>

#include <lgpio.h>

/* --- LED/GPIO helpers ---------------------------------------------------- */

static int daemon_led_chip = -1;
static int daemon_led_433_claimed = 0;
static int daemon_led_868_claimed = 0;

static int daemon_led_claim_pin(int pin)
{
    int rc = lgGpioClaimOutput(daemon_led_chip, 0, pin, 0);

    if (rc < 0)
        printf("[GPIO] Fehler: LED GPIO %d konnte nicht belegt werden: %d\n",
               pin, rc);

    return rc;
}

static void daemon_led_release_pin(int pin, int *claimed)
{
    if (daemon_led_chip < 0 || !claimed || !*claimed)
        return;

    lgGpioWrite(daemon_led_chip, pin, 0);
    lgGpioFree(daemon_led_chip, pin);
    *claimed = 0;
}

void daemon_led_shutdown(void)
{
    daemon_led_release_pin(DAEMON_LED_PIN_433, &daemon_led_433_claimed);
    daemon_led_release_pin(DAEMON_LED_PIN_868, &daemon_led_868_claimed);

    if (daemon_led_chip >= 0) {
        lgGpiochipClose(daemon_led_chip);
        daemon_led_chip = -1;
    }
}

void daemon_led_init(void)
{
    daemon_led_shutdown();

    daemon_led_chip = lgGpiochipOpen(0);

    if (daemon_led_chip < 0) {
        printf("[GPIO] Fehler: gpiochip0 konnte nicht geöffnet werden!\n");
        return;
    }

    if (daemon_led_claim_pin(DAEMON_LED_PIN_433) < 0 ||
        daemon_led_claim_pin(DAEMON_LED_PIN_868) < 0) {
        daemon_led_shutdown();
        return;
    }

    daemon_led_433_claimed = 1;
    daemon_led_868_claimed = 1;

    daemon_led_set_pin(DAEMON_LED_PIN_433, 0);
    daemon_led_set_pin(DAEMON_LED_PIN_868, 0);
}

int daemon_led_ready(void)
{
    return daemon_led_chip >= 0 &&
           daemon_led_433_claimed &&
           daemon_led_868_claimed ? 1 : 0;
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
