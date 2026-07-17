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

void config_status_trim_copy(char *dst,
                                           size_t dst_size,
                                           const char *src);

void config_status_uppercase(char *s);

int config_status_command_equals(const char *line,
                                               const char *expected);

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

int config_status_is_set_txresult(const char *line,
                                               int *enabled);


int config_status_is_set_txqueue(const char *line,
                                              int *enabled);

int config_status_is_set_cadmonitor(const char *line,
                                                  int *enabled);

int config_status_is_set_txmode(const char *line,
                                             RadioTxMode_t *mode);


int config_status_parse_set_uval(const char *cmd,
                                               const char *prefix,
                                               uint32_t lo, uint32_t hi,
                                               uint32_t *out);

int config_status_parse_set_ival(const char *cmd,
                                               const char *prefix,
                                               long lo, long hi,
                                               long *out);

int config_status_is_set_cadrssi(const char *line, int *dbm);

int config_status_is_set_cadwait(const char *line, uint32_t *ms);

int config_status_is_set_cadidle(const char *line, uint32_t *ms);

int config_status_is_set_cadpoll(const char *line, uint32_t *ms);

int config_status_is_set_cadtxaftertimeout(const char *line, int *val);

size_t config_status_txq_pending(const RadioController *ctrl);

size_t config_status_txq_dropped(const RadioController *ctrl);

size_t config_status_txq_rejected(const RadioController *ctrl);

size_t config_status_txq_stale(const RadioController *ctrl);


size_t config_status_txq_result_dropped(
    const RadioController *ctrl);


size_t config_status_txq_processed(const RadioController *ctrl);


int config_status_txq_last_result(const RadioController *ctrl,
                                                DaemonTxJobResult *result);

const char *config_status_txq_last_name(const RadioController *ctrl);

unsigned config_status_txq_last_seq(const RadioController *ctrl);

void config_status_format(char *buf,
                                        size_t buf_size,
                                        const RadioController *ctrl);

const char *config_status_cad_state_name(RadioCadProbeStatus status);

float config_status_live_rssi_dbm(RadioController *ctrl);

void config_status_format_channel(char *buf,
                                                size_t buf_size,
                                                RadioController *ctrl);

void config_status_format_stats(char *buf,
                                             size_t buf_size,
                                             const RadioController *ctrl);

#endif
