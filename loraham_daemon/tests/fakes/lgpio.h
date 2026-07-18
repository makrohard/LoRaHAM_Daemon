#ifndef LORAHAM_TEST_FAKE_LGPIO_H
#define LORAHAM_TEST_FAKE_LGPIO_H

#ifdef __cplusplus
extern "C" {
#endif

int lgGpiochipOpen(int gpio_dev);
int lgGpiochipClose(int handle);
int lgGpioClaimOutput(int handle, int flags, unsigned gpio, int level);
int lgGpioWrite(int handle, unsigned gpio, int level);
int lgGpioFree(int handle, unsigned gpio);

#ifdef __cplusplus
}
#endif

#endif
