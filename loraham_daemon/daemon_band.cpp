#include "daemon_band.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daemon_led.h"
#include "daemon_protocol.h"
#include "radio_rf_defaults.h"

/* --- Boot-RF-Defaults ------------------------------------------------------ */
// Per-band boot defaults, applied inside RadioDriver::begin() in the exact
// pre-driver setter order. LDRO: 433 = autoLDRO()+forceLDRO(1), 868 = only
// autoLDRO() (Feld ldro: >=0 forciert, <0 auto).

// LoRa-APRS:
static const RadioRfDefaults rf_defaults_433 = {
    433.900f,   /* freq_mhz */
    12,         /* spreading_factor */
    125.0f,     /* bandwidth_khz */
    0x12,       /* sync_word */
    8,          /* preamble_len */
    5,          /* coding_rate */
    true,       /* crc_on */
    1,          /* ldro: autoLDRO() + forceLDRO(1) */
    10          /* power_dbm */
};

/*
*
* // LoRa DX Cluster:
*    433.900 / SF10 / BW125.0 / Sync 0x12 / Preamble 8 / CR5 / CRC an /
*    autoLDRO() / 10 dBm
*
*
*
* // Meshtastic 433:
*
*    433.900 (DB0ARD) / SF11 / BW125.0 / Sync 0x2B / Preamble 16 / CR5 /
*    CRC an / autoLDRO() / 10 dBm
*
*
* // Meshcom:
*
*    433.175 / SF11 / BW250.0 / Sync 0x2B / Preamble 8 / CR6 / CRC an /
*    (kein autoLDRO) / 10 dBm
*/

static const RadioRfDefaults rf_defaults_868 = {
    869.525f,   /* freq_mhz */
    11,         /* spreading_factor */
    250.0f,     /* bandwidth_khz */
    0x2B,       /* sync_word */
    16,         /* preamble_len */
    5,          /* coding_rate */
    true,       /* crc_on */
    -1,         /* ldro: autoLDRO() only */
    10          /* power_dbm */
};

/* --- Band descriptors ------------------------------------------------------ */

static const DaemonBandDescriptor band_433 = {
    RADIO_BAND_433,
    433,
    "433",
    false,
    DATA433_SOCKET,
    DATA433_FRAMED_SOCKET,
    CONF433_SOCKET,
    "TX433",
    "TX433F",
    "RX433",
    "CONF433",
    "CONFIG433",
    "CLIENT433",
    "CAD433",
    "RSSI433",
    430.0f,
    440.0f,
    &rf_defaults_433,
    DAEMON_LED_PIN_433
};

static const DaemonBandDescriptor band_868 = {
    RADIO_BAND_868,
    868,
    "868",
    true,
    DATA868_SOCKET,
    DATA868_FRAMED_SOCKET,
    CONF868_SOCKET,
    "TX868",
    "TX868F",
    "RX868",
    "CONF868",
    "CONFIG868",
    "CLIENT868",
    "CAD868",
    "RSSI868",
    863.0f,
    870.0f,
    &rf_defaults_868,
    DAEMON_LED_PIN_868
};

static const DaemonBandDescriptor *g_band = NULL;

/* LORAHAM_SOCKET_DIR override (dev/test only; audit item 3) and the --id
 * names: resolved once, here, into static buffers -- every consumer keeps
 * reading the immutable descriptor. Empty/unset env and no --id means the
 * production /run/loraham band defaults, byte for byte. */
static DaemonBandDescriptor g_band_override;
static char g_sock_data[DAEMON_SOCKET_PATH_MAX + 1];
static char g_sock_framed[DAEMON_SOCKET_PATH_MAX + 1];
static char g_sock_conf[DAEMON_SOCKET_PATH_MAX + 1];
static char g_path_error[DAEMON_SOCKET_PATH_ERROR_MAX];   /* the whole path, up to PATH_MAX */

static char g_instance_id[17];
static bool g_instance_id_set = false;
static char g_daemon_log_path[64];

bool daemon_instance_id_valid(const char *id)
{
    size_t n;

    if (!id || id[0] < 'a' || id[0] > 'z')
        return false;
    n = strlen(id);
    if (n > 16)
        return false;
    for (size_t i = 1; i < n; ++i) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

bool daemon_set_instance_id(const char *id)
{
    if (!daemon_instance_id_valid(id))
        return false;
    snprintf(g_instance_id, sizeof(g_instance_id), "%s", id);
    g_instance_id_set = true;
    return true;
}

const char *daemon_instance_id(void)
{
    return g_instance_id_set ? g_instance_id : NULL;
}

void daemon_instance_id_reset(void)
{
    g_instance_id[0] = '\0';
    g_instance_id_set = false;
}

const char *daemon_band_daemon_log_path(void)
{
    if (!g_instance_id_set)
        return "/tmp/lora_daemon.log";
    snprintf(g_daemon_log_path, sizeof(g_daemon_log_path),
             "/tmp/lora_daemon-%s.log", g_instance_id);
    return g_daemon_log_path;
}

bool daemon_band_socket_paths_ok(char *why, size_t why_size)
{
    if (g_path_error[0] == '\0')
        return true;
    if (why && why_size > 0)
        snprintf(why, why_size, "%s", g_path_error);
    return false;
}

/* <dir>/<name> into `out`, or the reason into g_path_error. The length is
 * measured on the whole path before anything is copied. */
static void band_socket_path(char *out, const char *dir, const char *name)
{
    size_t need = strlen(dir) + 1 + strlen(name);

    if (need > DAEMON_SOCKET_PATH_MAX) {
        out[0] = '\0';
        if (g_path_error[0] == '\0')
            snprintf(g_path_error, sizeof(g_path_error),
                     "socket path too long (%zu bytes, at most %d): %s/%s",
                     need, DAEMON_SOCKET_PATH_MAX, dir, name);
        return;
    }
    memcpy(out, dir, strlen(dir));
    out[strlen(dir)] = '/';
    memcpy(out + strlen(dir) + 1, name, strlen(name) + 1);
}

void daemon_band_resolve(RadioBand_t band)
{
    const DaemonBandDescriptor *table =
        (band == RADIO_BAND_868) ? &band_868 : &band_433;
    const char *dir = getenv("LORAHAM_SOCKET_DIR");
    char name_data[48];
    char name_framed[48];
    char name_conf[48];

    g_path_error[0] = '\0';

    if ((!dir || dir[0] == '\0') && !g_instance_id_set) {
        g_band = table;
        return;
    }
    if (!dir || dir[0] == '\0')
        dir = LORAHAM_SOCKET_DIR_DEFAULT;

    if (g_instance_id_set) {
        snprintf(name_data, sizeof(name_data), DATA_ID_SOCKET_FMT, g_instance_id);
        snprintf(name_framed, sizeof(name_framed), DATA_ID_FRAMED_SOCKET_FMT, g_instance_id);
        snprintf(name_conf, sizeof(name_conf), CONF_ID_SOCKET_FMT, g_instance_id);
    } else {
        snprintf(name_data, sizeof(name_data), "%s",
                 band == RADIO_BAND_868 ? DATA868_SOCKET_NAME : DATA433_SOCKET_NAME);
        snprintf(name_framed, sizeof(name_framed), "%s",
                 band == RADIO_BAND_868 ? DATA868_FRAMED_SOCKET_NAME
                                        : DATA433_FRAMED_SOCKET_NAME);
        snprintf(name_conf, sizeof(name_conf), "%s",
                 band == RADIO_BAND_868 ? CONF868_SOCKET_NAME : CONF433_SOCKET_NAME);
    }

    g_band_override = *table;
    band_socket_path(g_sock_data, dir, name_data);
    band_socket_path(g_sock_framed, dir, name_framed);
    band_socket_path(g_sock_conf, dir, name_conf);
    g_band_override.data_socket = g_sock_data;
    g_band_override.framed_socket = g_sock_framed;
    g_band_override.conf_socket = g_sock_conf;
    g_band = &g_band_override;
}

const DaemonBandDescriptor *daemon_band(void)
{
    if (!g_band) {
        /* Fail closed: reading the band before --radio resolution is a
         * programming error, never a recoverable runtime state. */
        fprintf(stderr, "[Daemon] daemon_band() read before resolution\n");
        abort();
    }

    return g_band;
}
