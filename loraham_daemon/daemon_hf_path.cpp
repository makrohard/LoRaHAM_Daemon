#include "daemon_hf_path.h"

#include <string.h>
#include <unistd.h>

static void sleep_usleep(unsigned us)
{
    usleep(us);
}

static void (*g_sleep_us)(unsigned) = sleep_usleep;

bool daemon_hf_path(RadioBand_t band)
{
    return band == RADIO_BAND_868 || band == RADIO_BAND_915;
}

void daemon_rx_clear_after_read(RadioBand_t band, uint8_t *buf, size_t len)
{
    if (daemon_hf_path(band) && buf && len)
        memset(buf, 0, len);
}

unsigned daemon_tx_guard_us(RadioBand_t band)
{
    return daemon_hf_path(band) ? DAEMON_TX_GUARD_US : 0u;
}

void daemon_tx_guard_wait(RadioBand_t band)
{
    unsigned us = daemon_tx_guard_us(band);

    if (us)
        g_sleep_us(us);
}

void daemon_tx_guard_set_sleep(void (*sleep_us)(unsigned))
{
    g_sleep_us = sleep_us ? sleep_us : sleep_usleep;
}
