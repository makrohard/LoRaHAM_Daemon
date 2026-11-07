/*
 * The HF path (daemon_hf_path.h): what 868 and 915 do and 433 does not. After
 * an RX read the buffer is cleared; after a TX the daemon waits
 * DAEMON_TX_GUARD_US. The wait goes through a test hook, so it is counted,
 * never timed.
 */

#include "../daemon_hf_path.h"

#include <cstdio>
#include <cstring>

static int g_ok = 0;
static int g_fail = 0;

static void expect(const char *name, bool ok)
{
    if (ok) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
}

static unsigned g_sleeps, g_slept_us;
static void fake_sleep(unsigned us)
{
    g_sleeps++;
    g_slept_us += us;
}

static void test_band(RadioBand_t band, bool hf)
{
    char label[96];
    uint8_t buf[16];

    snprintf(label, sizeof(label), "%d: HF path %s", (int)band, hf ? "yes" : "no");
    expect(label, daemon_hf_path(band) == hf);

    memset(buf, 0xAA, sizeof(buf));
    daemon_rx_clear_after_read(band, buf, sizeof(buf));
    bool cleared = true, untouched = true;
    for (uint8_t b : buf) {
        cleared = cleared && b == 0;
        untouched = untouched && b == 0xAA;
    }
    snprintf(label, sizeof(label), "%d: RX buffer %s after the read", (int)band, hf ? "cleared" : "untouched");
    expect(label, hf ? cleared : untouched);

    memset(buf, 0xAA, sizeof(buf));
    daemon_rx_clear_after_read(band, buf, 4);
    snprintf(label, sizeof(label), "%d: RX clear stays inside its length", (int)band);
    expect(label, buf[4] == 0xAA && buf[15] == 0xAA && (hf ? buf[3] == 0 : buf[3] == 0xAA));

    snprintf(label, sizeof(label), "%d: TX guard %u us", (int)band, hf ? DAEMON_TX_GUARD_US : 0u);
    expect(label, daemon_tx_guard_us(band) == (hf ? DAEMON_TX_GUARD_US : 0u));

    g_sleeps = g_slept_us = 0;
    daemon_tx_guard_wait(band);
    snprintf(label, sizeof(label), "%d: the TX guard %s", (int)band, hf ? "waits once, 50 ms" : "does not wait");
    expect(label, hf ? (g_sleeps == 1 && g_slept_us == 50000u) : g_sleeps == 0);
}

int main(void)
{
    daemon_tx_guard_set_sleep(fake_sleep);
    test_band(RADIO_BAND_433, false);
    test_band(RADIO_BAND_868, true);
    test_band(RADIO_BAND_915, true);
    test_band(static_cast<RadioBand_t>(0), false);   /* no band: never the HF path */
    daemon_rx_clear_after_read(RADIO_BAND_868, nullptr, 0);
    expect("RX clear of an empty buffer is a no-op", true);
    printf("\nSummary: ok=%d fail=%d\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
