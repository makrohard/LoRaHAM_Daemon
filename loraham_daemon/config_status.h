#ifndef LORAHAM_CONFIG_STATUS_H
#define LORAHAM_CONFIG_STATUS_H

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

#include "daemon_stats.h"
#include "daemon_tx_async_runtime.h"
#include "daemon_timing.h"
#include "radio_controller.h"
#include "radio_health.h"
#include "radio_cad.h"

/* --- CONF status queries ------------------------------------------------- */

static inline void config_status_trim_copy(char *dst,
                                           size_t dst_size,
                                           const char *src)
{
    size_t start = 0;
    size_t end = 0;
    size_t len;

    if (!dst || dst_size == 0)
        return;

    dst[0] = '\0';

    if (!src)
        return;

    len = strlen(src);
    while (start < len &&
           (src[start] == ' ' || src[start] == '\t' ||
            src[start] == '\r' || src[start] == '\n')) {
        start++;
    }

    end = len;
    while (end > start &&
           (src[end - 1] == ' ' || src[end - 1] == '\t' ||
            src[end - 1] == '\r' || src[end - 1] == '\n')) {
        end--;
    }

    len = end - start;
    if (len >= dst_size)
        len = dst_size - 1;

    memcpy(dst, src + start, len);
    dst[len] = '\0';
}

static inline void config_status_uppercase(char *s)
{
    if (!s)
        return;

    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

static inline int config_status_command_equals(const char *line,
                                               const char *expected)
{
    char cmd[64];

    config_status_trim_copy(cmd, sizeof(cmd), line);
    config_status_uppercase(cmd);

    return strcmp(cmd, expected) == 0;
}

static inline int config_status_is_get_status(const char *line)
{
    return config_status_command_equals(line, "GET STATUS");
}

static inline int config_status_is_get_stats(const char *line)
{
    return config_status_command_equals(line, "GET STATS");
}

static inline int config_status_is_get_channel(const char *line)
{
    return config_status_command_equals(line, "GET CHANNEL");
}

static inline int config_status_is_set_txresult(const char *line,
                                               int *enabled)
{
    char cmd[64];

    config_status_trim_copy(cmd, sizeof(cmd), line);
    config_status_uppercase(cmd);

    if (strcmp(cmd, "SET TXRESULT=1") == 0) {
        if (enabled)
            *enabled = 1;
        return 1;
    }

    if (strcmp(cmd, "SET TXRESULT=0") == 0) {
        if (enabled)
            *enabled = 0;
        return 1;
    }

    return 0;
}


static inline int config_status_is_set_txqueue(const char *line,
                                              int *enabled)
{
    char cmd[64];

    config_status_trim_copy(cmd, sizeof(cmd), line);
    config_status_uppercase(cmd);

    if (strcmp(cmd, "SET TXQUEUE=1") == 0) {
        if (enabled)
            *enabled = 1;
        return 1;
    }

    if (strcmp(cmd, "SET TXQUEUE=0") == 0) {
        if (enabled)
            *enabled = 0;
        return 1;
    }

    return 0;
}

static inline int config_status_is_set_txmode(const char *line,
                                             RadioTxMode_t *mode)
{
    char cmd[64];

    config_status_trim_copy(cmd, sizeof(cmd), line);
    config_status_uppercase(cmd);

    if (strcmp(cmd, "SET TXMODE=MANAGED") == 0) {
        if (mode)
            *mode = RADIO_TX_MODE_MANAGED;
        return 1;
    }

    if (strcmp(cmd, "SET TXMODE=RAW") == 0) {
        if (mode)
            *mode = RADIO_TX_MODE_RAW;
        return 1;
    }

    return 0;
}


static inline int config_status_parse_set_uval(const char *cmd,
                                               const char *prefix,
                                               uint32_t lo, uint32_t hi,
                                               uint32_t *out)
{
    size_t plen;
    char *endptr;
    unsigned long v;

    if (!cmd || !prefix || cmd[0] == '\0' || prefix[0] == '\0')
        return 0;

    plen = strlen(prefix);
    if (strncmp(cmd, prefix, plen) != 0)
        return 0;

    if (cmd[plen] == '\0')
        return 0;

    v = strtoul(cmd + plen, &endptr, 10);
    if (*endptr != '\0' || v < (unsigned long)lo || v > (unsigned long)hi)
        return 0;

    if (out)
        *out = (uint32_t)v;
    return 1;
}

static inline int config_status_is_set_cadwait(const char *line, uint32_t *ms)
{
    char cmd[64];

    config_status_trim_copy(cmd, sizeof(cmd), line);
    config_status_uppercase(cmd);
    return config_status_parse_set_uval(cmd, "SET CADWAIT=", 50u, 5000u, ms);
}

static inline int config_status_is_set_cadidle(const char *line, uint32_t *ms)
{
    char cmd[64];

    config_status_trim_copy(cmd, sizeof(cmd), line);
    config_status_uppercase(cmd);
    return config_status_parse_set_uval(cmd, "SET CADIDLE=", 0u, 2000u, ms);
}

static inline int config_status_is_set_cadpoll(const char *line, uint32_t *ms)
{
    char cmd[64];

    config_status_trim_copy(cmd, sizeof(cmd), line);
    config_status_uppercase(cmd);
    return config_status_parse_set_uval(cmd, "SET CADPOLL=", 10u, 500u, ms);
}

static inline int config_status_is_set_cadtxaftertimeout(const char *line, int *val)
{
    char cmd[64];

    config_status_trim_copy(cmd, sizeof(cmd), line);
    config_status_uppercase(cmd);

    if (strcmp(cmd, "SET CADTXAFTERTIMEOUT=1") == 0) {
        if (val)
            *val = 1;
        return 1;
    }

    if (strcmp(cmd, "SET CADTXAFTERTIMEOUT=0") == 0) {
        if (val)
            *val = 0;
        return 1;
    }

    return 0;
}

template<typename RadioT>
static inline size_t config_status_txq_pending(const RadioController<RadioT> *ctrl)
{
    if (!ctrl || !ctrl->tx_queue_active.load())
        return 0;

    return daemon_tx_async_runtime_pending_for_band(
        radio_controller_band_number(ctrl));
}

template<typename RadioT>
static inline size_t config_status_txq_dropped(const RadioController<RadioT> *ctrl)
{
    if (!ctrl || !ctrl->tx_queue_active.load())
        return 0;

    return daemon_tx_async_runtime_dropped_for_band(
        radio_controller_band_number(ctrl));
}

template<typename RadioT>
static inline size_t config_status_txq_rejected(const RadioController<RadioT> *ctrl)
{
    if (!ctrl || !ctrl->tx_queue_active.load())
        return 0;

    return daemon_tx_async_runtime_rejected_for_band(
        radio_controller_band_number(ctrl));
}

template<typename RadioT>
static inline size_t config_status_txq_stale(const RadioController<RadioT> *ctrl)
{
    if (!ctrl || !ctrl->tx_queue_active.load())
        return 0;

    return daemon_tx_async_runtime_completion_stale_for_band(
        radio_controller_band_number(ctrl));
}


template<typename RadioT>
static inline size_t config_status_txq_result_dropped(
    const RadioController<RadioT> *ctrl)
{
    if (!ctrl || !ctrl->tx_queue_active.load())
        return 0;

    return daemon_tx_async_runtime_completion_dropped_for_band(
        radio_controller_band_number(ctrl));
}


template<typename RadioT>
static inline size_t config_status_txq_processed(const RadioController<RadioT> *ctrl)
{
    if (!ctrl || !ctrl->tx_queue_active.load())
        return 0;

    return daemon_tx_async_runtime_processed_for_band(
        radio_controller_band_number(ctrl));
}


template<typename RadioT>
static inline int config_status_txq_last_result(const RadioController<RadioT> *ctrl,
                                                DaemonTxJobResult *result)
{
    if (!ctrl || !result)
        return 0;

    if (!ctrl->tx_queue_active.load())
        return 0;

    return daemon_tx_async_runtime_last_result_for_band(radio_controller_band_number(ctrl),
                                                       result);
}

template<typename RadioT>
static inline const char *config_status_txq_last_name(const RadioController<RadioT> *ctrl)
{
    DaemonTxJobResult result;

    if (!config_status_txq_last_result(ctrl, &result))
        return "NONE";

    return tx_result_name(result.tx_result);
}

template<typename RadioT>
static inline unsigned config_status_txq_last_seq(const RadioController<RadioT> *ctrl)
{
    DaemonTxJobResult result;

    if (!config_status_txq_last_result(ctrl, &result))
        return 0;

    return result.seq;
}

template<typename RadioT>
static inline void config_status_format(char *buf,
                                        size_t buf_size,
                                        const RadioController<RadioT> *ctrl)
{
    snprintf(buf,
             buf_size,
             "STATUS RADIO=%s TX=%d CAD=%d GETRSSI=%d TXRESULT=%d TXMODE=%s TXQUEUE=%d TXQ=%zu TXQDROP=%zu TXQREJECT=%zu TXQSTALE=%zu TXQRESULTDROP=%zu TXQDONE=%zu TXQLAST=%s TXQSEQ=%u CADWAIT=%u CADIDLE=%u CADPOLL=%u CADTXAFTERTIMEOUT=%d\n",
             radio_health_name(radio_controller_health(ctrl)),
             (ctrl && ctrl->tx_busy.load()) ? 1 : 0,
             (ctrl && ctrl->cad_broadcast_active.load()) ? 1 : 0,
             (ctrl && ctrl->getrssi_active.load()) ? 1 : 0,
             (ctrl && ctrl->tx_result_active.load()) ? 1 : 0,
             radio_tx_mode_name(ctrl ? ctrl->tx_mode : RADIO_TX_MODE_MANAGED),
             (ctrl && ctrl->tx_queue_active.load()) ? 1 : 0,
             config_status_txq_pending(ctrl),
             config_status_txq_dropped(ctrl),
             config_status_txq_rejected(ctrl),
             config_status_txq_stale(ctrl),
             config_status_txq_result_dropped(ctrl),
             config_status_txq_processed(ctrl),
             config_status_txq_last_name(ctrl),
             config_status_txq_last_seq(ctrl),
             ctrl ? (unsigned)ctrl->cad_wait_timeout_ms.load()
                  : DAEMON_TX_POLICY_CAD_WAIT_TIMEOUT_MS,
             ctrl ? (unsigned)ctrl->cad_idle_stable_ms.load()
                  : DAEMON_TX_POLICY_CAD_IDLE_STABLE_MS,
             ctrl ? (unsigned)ctrl->cad_poll_interval_ms.load()
                  : DAEMON_TX_POLICY_POLL_INTERVAL_MS,
             (ctrl && ctrl->cad_send_after_timeout.load()) ? 1 : 0);
}

static inline const char *config_status_cad_state_name(RadioCadProbeStatus status)
{
    if (status == RADIO_CAD_PROBE_FREE)
        return "FREE";

    if (status == RADIO_CAD_PROBE_BUSY)
        return "BUSY";

    return "UNAVAILABLE";
}

template<typename RadioT>
static inline float config_status_live_rssi_dbm(RadioController<RadioT> *ctrl)
{
    if (!ctrl || !ctrl->mod || ctrl->tx_busy.load())
        return -200.0f;

    std::unique_lock<std::recursive_mutex> radio_lock(
        ctrl->radio_mutex, std::try_to_lock);
    if (!radio_lock.owns_lock() || ctrl->tx_busy.load())
        return -200.0f;

    uint8_t reg = (ctrl->mode == RADIO_MODE_LORA) ? 0x1B : 0x11;
    int16_t raw = ctrl->mod->SPIgetRegValue(reg, 7, 0);

    if (raw < 0)
        return -200.0f;

    if (ctrl->mode == RADIO_MODE_LORA)
        return (ctrl->is_hf ? -157.0f : -164.0f) + (float)raw;

    return -((float)raw) / 2.0f;
}

template<typename RadioT>
static inline void config_status_format_channel(char *buf,
                                                size_t buf_size,
                                                RadioController<RadioT> *ctrl)
{
    int tx_busy = (ctrl && ctrl->tx_busy.load()) ? 1 : 0;
    RadioCadProbeResult probe = radio_cad_try_probe(ctrl);
    float live_rssi = config_status_live_rssi_dbm(ctrl);

    snprintf(buf,
             buf_size,
             "CHANNEL RADIO=%s BUSY=%d CAD=%d CADSCAN=%d CADSTATE=%s "
             "RSSI=%.2f PACKETRSSI=%.2f LIVERSSI=%.2f MODE=%s TXMODE=%s\n",
             radio_health_name(radio_controller_health(ctrl)),
             tx_busy || probe.status == RADIO_CAD_PROBE_BUSY ? 1 : 0,
             probe.scan_ran ? 1 : 0,
             probe.scan_ran ? 1 : 0,
             config_status_cad_state_name(probe.status),
             probe.rssi_dbm,
             probe.rssi_dbm,
             live_rssi,
             radio_mode_name(radio_controller_mode(ctrl)),
             radio_tx_mode_name(ctrl ? ctrl->tx_mode : RADIO_TX_MODE_MANAGED));
}

template<typename RadioT>
static inline void config_status_format_stats(char *buf,
                                             size_t buf_size,
                                             const RadioController<RadioT> *ctrl)
{
    long uptime = daemon_stats_uptime_seconds(daemon_now_ms());

    daemon_stats_format_response(buf,
                                 buf_size,
                                 uptime,
                                 radio_controller_health(ctrl),
                                 ctrl ? &ctrl->stats : NULL);
}

#endif
