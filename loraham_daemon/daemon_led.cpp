#include "daemon_led.h"

#include <stdio.h>

#include <lgpio.h>

#include "daemon_radio_selection.h"

/* --- LED/GPIO helpers ---------------------------------------------------- */
/*
 * The status LED is a per-band hardware resource (a GPIO line) and an activity
 * indicator -- it is NOT the process-instance ownership lock. Same-band
 * ownership and duplicate-instance rejection are handled by the per-band FD
 * locks in daemon_instance_lock (instance-433.lock / instance-868.lock).
 *
 * The LED line is claimed only for the single band selected via --radio, and a
 * failed claim of the *selected* band is treated as fatal by the caller because
 * the LED is a required hardware resource for that band.
 */

static int daemon_led_chip = -1;
static int daemon_led_433_claimed = 0;
static int daemon_led_868_claimed = 0;

static int daemon_led_claim_pin(int pin, int *claimed)
{
    int rc;

    if (!claimed)
        return -1;

    rc = lgGpioClaimOutput(daemon_led_chip, 0, pin, 0);

    if (rc < 0) {
        printf("[GPIO] Fehler: LED GPIO %d konnte nicht belegt werden: %d\n",
               pin, rc);
        return rc;
    }

    *claimed = 1;
    return 0;
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

static int daemon_led_claim_band(bool enabled, int pin, int *claimed,
                                 const char *band)
{
    if (!enabled)
        return 0;

    if (daemon_led_claim_pin(pin, claimed) < 0) {
        printf("[GPIO] Fehler: LED/Funk für Band %s bereits in Benutzung "
               "(GPIO %d belegt) – andere Instanz besitzt dieses Band\n",
               band, pin);
        return -1;
    }

    return 0;
}

int daemon_led_init(void)
{
    daemon_led_shutdown();

    daemon_led_chip = lgGpiochipOpen(0);

    if (daemon_led_chip < 0) {
        printf("[GPIO] Fehler: gpiochip0 konnte nicht geöffnet werden!\n");
        return -1;
    }

    /* Claim the LED line only for the selected band. */
    if (daemon_led_claim_band(daemon_radio_433_enabled(),
                              DAEMON_LED_PIN_433,
                              &daemon_led_433_claimed, "433") < 0 ||
        daemon_led_claim_band(daemon_radio_868_enabled(),
                              DAEMON_LED_PIN_868,
                              &daemon_led_868_claimed, "868") < 0) {
        daemon_led_shutdown();
        return -1;
    }

    if (daemon_radio_433_enabled())
        daemon_led_set_pin(DAEMON_LED_PIN_433, 0);
    if (daemon_radio_868_enabled())
        daemon_led_set_pin(DAEMON_LED_PIN_868, 0);

    return 0;
}

int daemon_led_ready(void)
{
    if (daemon_led_chip < 0)
        return 0;

    /* Only the selected band must hold its LED line. */
    if (daemon_radio_433_enabled() && !daemon_led_433_claimed)
        return 0;

    if (daemon_radio_868_enabled() && !daemon_led_868_claimed)
        return 0;

    return 1;
}

void daemon_led_set_pin(int pin, int state)
{
    if (daemon_led_ready())
        lgGpioWrite(daemon_led_chip, pin, state ? 1 : 0);
}
