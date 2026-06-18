#include "daemon_stats.h"

#include <stdio.h>
#include <string.h>

/* --- Runtime statistics -------------------------------------------------- */

static long g_stats_start_ms = 0;

void daemon_radio_stats_init(DaemonRadioStats *stats)
{
    if (!stats)
        return;

    memset(stats, 0, sizeof(*stats));
}

void daemon_radio_stats_record_rx(DaemonRadioStats *stats, size_t bytes)
{
    if (!stats)
        return;

    stats->rx_packets++;
    stats->rx_bytes += (unsigned long)bytes;
}

void daemon_radio_stats_record_rx_drop(DaemonRadioStats *stats)
{
    if (!stats)
        return;

    stats->rx_drops++;
}

void daemon_radio_stats_record_tx_result(DaemonRadioStats *stats,
                                         TxResult result)
{
    if (!stats)
        return;

    switch (result) {
        case TX_RESULT_OK:
            stats->tx_ok++;
            break;
        case TX_RESULT_BUSY:
            stats->tx_busy++;
            break;
        case TX_RESULT_CAD_TIMEOUT:
            stats->cad_timeouts++;
            break;
        case TX_RESULT_INVALID_BAND:
        case TX_RESULT_INVALID_PACKET:
        case TX_RESULT_RADIO_NOT_READY:
        case TX_RESULT_RADIO_ERROR:
            stats->tx_errors++;
            break;
    }
}

void daemon_radio_stats_record_cad_timeout(DaemonRadioStats *stats)
{
    if (!stats)
        return;

    stats->cad_timeouts++;
}


void daemon_radio_stats_record_cad_timeout_send(DaemonRadioStats *stats)
{
    if (!stats)
        return;

    stats->cad_timeout_sends++;
}

void daemon_stats_start(long now_ms)
{
    g_stats_start_ms = now_ms;
}

long daemon_stats_uptime_seconds(long now_ms)
{
    if (g_stats_start_ms <= 0 || now_ms < g_stats_start_ms)
        return 0;

    return (now_ms - g_stats_start_ms) / 1000L;
}

void daemon_stats_format_fields(char *buf,
                                size_t buf_size,
                                long uptime_seconds,
                                RadioHealth health,
                                const DaemonRadioStats *stats)
{
    DaemonRadioStats empty;

    if (!buf || buf_size == 0)
        return;

    if (!stats) {
        daemon_radio_stats_init(&empty);
        stats = &empty;
    }

    snprintf(buf,
             buf_size,
             "UPTIME=%ld RADIO=%s RX=%lu RXBYTES=%lu RXDROPS=%lu "
             "TXOK=%lu TXERR=%lu TXBUSY=%lu CADTIMEOUT=%lu CADSEND=%lu",
             uptime_seconds,
             radio_health_name(health),
             stats->rx_packets,
             stats->rx_bytes,
             stats->rx_drops,
             stats->tx_ok,
             stats->tx_errors,
             stats->tx_busy,
             stats->cad_timeouts,
             stats->cad_timeout_sends);
}

void daemon_stats_format_response(char *buf,
                                  size_t buf_size,
                                  long uptime_seconds,
                                  RadioHealth health,
                                  const DaemonRadioStats *stats)
{
    char fields[256];

    if (!buf || buf_size == 0)
        return;

    daemon_stats_format_fields(fields,
                               sizeof(fields),
                               uptime_seconds,
                               health,
                               stats);

    snprintf(buf, buf_size, "STATS %s\n", fields);
}
