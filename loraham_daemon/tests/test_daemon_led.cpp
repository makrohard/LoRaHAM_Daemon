#include "../daemon_led.h"

#include <stdio.h>
#include <string.h>

/* --- LED-Tests ------------------------------------------------------------ */

static int g_ok = 0;
static int g_fail = 0;

static int g_open_result = 7;
static int g_claim_433_result = 0;
static int g_claim_868_result = 0;

static int g_open_count = 0;
static int g_close_count = 0;
static int g_claim_count = 0;
static int g_write_count = 0;
static int g_free_count = 0;

static unsigned g_claim_pins[4];
static int g_claim_levels[4];
static unsigned g_write_pins[8];
static int g_write_levels[8];
static unsigned g_free_pins[4];

extern "C" int lgGpiochipOpen(int gpio_dev)
{
    (void)gpio_dev;
    g_open_count++;
    return g_open_result;
}

extern "C" int lgGpiochipClose(int handle)
{
    (void)handle;
    g_close_count++;
    return 0;
}

extern "C" int lgGpioClaimOutput(int handle, int flags,
                                 unsigned gpio, int level)
{
    (void)handle;
    (void)flags;

    if (g_claim_count < (int)(sizeof(g_claim_pins) / sizeof(g_claim_pins[0]))) {
        g_claim_pins[g_claim_count] = gpio;
        g_claim_levels[g_claim_count] = level;
    }
    g_claim_count++;

    if (gpio == DAEMON_LED_PIN_433)
        return g_claim_433_result;

    if (gpio == DAEMON_LED_PIN_868)
        return g_claim_868_result;

    return -99;
}

extern "C" int lgGpioWrite(int handle, unsigned gpio, int level)
{
    (void)handle;

    if (g_write_count < (int)(sizeof(g_write_pins) / sizeof(g_write_pins[0]))) {
        g_write_pins[g_write_count] = gpio;
        g_write_levels[g_write_count] = level;
    }
    g_write_count++;
    return 0;
}

extern "C" int lgGpioFree(int handle, unsigned gpio)
{
    (void)handle;

    if (g_free_count < (int)(sizeof(g_free_pins) / sizeof(g_free_pins[0])))
        g_free_pins[g_free_count] = gpio;
    g_free_count++;
    return 0;
}

static void expect_int(const char *name, int actual, int expected)
{
    if (actual == expected) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s: expected %d, got %d\n", name, expected, actual);
    }
}

static void reset_fake(void)
{
    daemon_led_shutdown();

    g_open_result = 7;
    g_claim_433_result = 0;
    g_claim_868_result = 0;

    g_open_count = 0;
    g_close_count = 0;
    g_claim_count = 0;
    g_write_count = 0;
    g_free_count = 0;

    memset(g_claim_pins, 0, sizeof(g_claim_pins));
    memset(g_claim_levels, 0, sizeof(g_claim_levels));
    memset(g_write_pins, 0, sizeof(g_write_pins));
    memset(g_write_levels, 0, sizeof(g_write_levels));
    memset(g_free_pins, 0, sizeof(g_free_pins));
}

static void test_successful_lifecycle(void)
{
    reset_fake();

    daemon_led_init();

    expect_int("LED ready after init", daemon_led_ready(), 1);
    expect_int("LED opens one chip", g_open_count, 1);
    expect_int("LED claims two pins", g_claim_count, 2);
    expect_int("LED claims GPIO13", (int)g_claim_pins[0], DAEMON_LED_PIN_433);
    expect_int("LED claims GPIO19", (int)g_claim_pins[1], DAEMON_LED_PIN_868);
    expect_int("LED GPIO13 initial low", g_claim_levels[0], 0);
    expect_int("LED GPIO19 initial low", g_claim_levels[1], 0);

    daemon_led_set_pin(DAEMON_LED_PIN_433, 1);
    expect_int("LED write after init", g_write_count, 3);
    expect_int("LED write GPIO13", (int)g_write_pins[2], DAEMON_LED_PIN_433);
    expect_int("LED write on", g_write_levels[2], 1);

    daemon_led_shutdown();

    expect_int("LED not ready after shutdown", daemon_led_ready(), 0);
    expect_int("LED shutdown frees both", g_free_count, 2);
    expect_int("LED shutdown frees GPIO13", (int)g_free_pins[0], DAEMON_LED_PIN_433);
    expect_int("LED shutdown frees GPIO19", (int)g_free_pins[1], DAEMON_LED_PIN_868);
    expect_int("LED shutdown closes chip", g_close_count, 1);
    expect_int("LED shutdown final write count", g_write_count, 5);
    expect_int("LED shutdown GPIO19 low", (int)g_write_pins[4], DAEMON_LED_PIN_868);
    expect_int("LED shutdown level low", g_write_levels[4], 0);
}

static void test_second_claim_failure_cleans_first(void)
{
    reset_fake();
    g_claim_868_result = -6;

    daemon_led_init();

    expect_int("second claim leaves LED not ready", daemon_led_ready(), 0);
    expect_int("second claim tries both pins", g_claim_count, 2);
    expect_int("second claim frees first pin", g_free_count, 1);
    expect_int("second claim freed GPIO13", (int)g_free_pins[0], DAEMON_LED_PIN_433);
    expect_int("second claim forces GPIO13 low", g_write_count, 1);
    expect_int("second claim low GPIO13", (int)g_write_pins[0], DAEMON_LED_PIN_433);
    expect_int("second claim low level", g_write_levels[0], 0);
    expect_int("second claim closes chip", g_close_count, 1);
}

static void test_first_claim_failure_stops_cleanly(void)
{
    reset_fake();
    g_claim_433_result = -7;

    daemon_led_init();

    expect_int("first claim leaves LED not ready", daemon_led_ready(), 0);
    expect_int("first claim stops second claim", g_claim_count, 1);
    expect_int("first claim has no pin free", g_free_count, 0);
    expect_int("first claim has no pin write", g_write_count, 0);
    expect_int("first claim closes chip", g_close_count, 1);
}

static void test_chip_open_failure(void)
{
    reset_fake();
    g_open_result = -1;

    daemon_led_init();

    expect_int("chip open failure leaves LED not ready", daemon_led_ready(), 0);
    expect_int("chip open failure tries once", g_open_count, 1);
    expect_int("chip open failure claims nothing", g_claim_count, 0);
    expect_int("chip open failure closes nothing", g_close_count, 0);
}

int main(void)
{
    test_successful_lifecycle();
    test_second_claim_failure_cleans_first();
    test_first_claim_failure_stops_cleanly();
    test_chip_open_failure();

    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
