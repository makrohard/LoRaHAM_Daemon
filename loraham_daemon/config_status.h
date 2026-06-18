#ifndef LORAHAM_CONFIG_STATUS_H
#define LORAHAM_CONFIG_STATUS_H

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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


template<typename RadioT>
static inline size_t config_status_txq_pending(const RadioController<RadioT> *ctrl)
{
    if (!ctrl)
        return 0;

    if (ctrl->tx_queue_active.load())
        return daemon_tx_async_runtime_pending_for_band(radio_controller_band_number(ctrl));

    return daemon_tx_worker_pending(&ctrl->tx_worker);
}

template<typename RadioT>
static inline size_t config_status_txq_dropped(const RadioController<RadioT> *ctrl)
{
    if (!ctrl)
        return 0;

    if (ctrl->tx_queue_active.load())
        return daemon_tx_async_runtime_dropped_for_band(radio_controller_band_number(ctrl));

    return daemon_tx_worker_dropped(&ctrl->tx_worker);
}

template<typename RadioT>
static inline size_t config_status_txq_processed(const RadioController<RadioT> *ctrl)
{
    if (!ctrl)
        return 0;

    if (ctrl->tx_queue_active.load())
        return daemon_tx_async_runtime_processed_for_band(radio_controller_band_number(ctrl));

    return daemon_tx_worker_processed(&ctrl->tx_worker);
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
             "STATUS RADIO=%s TX=%d CAD=%d GETRSSI=%d TXRESULT=%d TXMODE=%s TXQUEUE=%d TXQ=%zu TXQDROP=%zu TXQDONE=%zu TXQLAST=%s TXQSEQ=%u\n",
             radio_health_name(radio_controller_health(ctrl)),
             (ctrl && ctrl->tx_busy.load()) ? 1 : 0,
             (ctrl && ctrl->cad_active.load()) ? 1 : 0,
             (ctrl && ctrl->getrssi_active.load()) ? 1 : 0,
             (ctrl && ctrl->tx_result_active.load()) ? 1 : 0,
             radio_tx_mode_name(ctrl ? ctrl->tx_mode : RADIO_TX_MODE_MANAGED),
             (ctrl && ctrl->tx_queue_active.load()) ? 1 : 0,
             config_status_txq_pending(ctrl),
             config_status_txq_dropped(ctrl),
             config_status_txq_processed(ctrl),
             config_status_txq_last_name(ctrl),
             config_status_txq_last_seq(ctrl));
}

template<typename RadioT>
static inline void config_status_format_channel(char *buf,
                                                size_t buf_size,
                                                RadioController<RadioT> *ctrl)
{
    RadioCadProbeResult probe = radio_cad_probe(ctrl);

    snprintf(buf,
             buf_size,
             "CHANNEL RADIO=%s BUSY=%d CAD=%d RSSI=%.2f MODE=%s TXMODE=%s\n",
             radio_health_name(radio_controller_health(ctrl)),
             probe.status == RADIO_CAD_PROBE_BUSY ? 1 : 0,
             probe.scan_ran ? 1 : 0,
             probe.rssi_dbm,
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
