#define _POSIX_C_SOURCE 200809L
#include "bridge_loraham.h"

#include "bridge_kiss.h"
#include "bridge_runtime.h"
#include "tcp_server.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * LoRaHAM daemon transport and TX lifecycle.
 * Owns CONF/DATA socket helpers, TX confirmation, RX restore,
 * and draining the outgoing TX queue into framed DATA packets.
 */

#define LHKT_TX_RESTORE_RETRY_DELAY_MS 100
#define LHKT_TX_START_WAIT_MS 500
#define LHKT_TEST_HOOK_MAX_CALLS 16

#ifdef LHKT_TEST
static int lhkt_test_config_results[LHKT_TEST_HOOK_MAX_CALLS];
static size_t lhkt_test_config_result_count;
static size_t lhkt_test_config_result_pos;
static double lhkt_test_config_freqs[LHKT_TEST_HOOK_MAX_CALLS];
static size_t lhkt_test_config_call_count;
static int lhkt_test_write_enabled;
static ssize_t lhkt_test_write_result;
static size_t lhkt_test_write_call_count;

void lhkt_test_bridge_reset_tx_hooks(void)
{
    memset(lhkt_test_config_results, 0, sizeof(lhkt_test_config_results));
    memset(lhkt_test_config_freqs, 0, sizeof(lhkt_test_config_freqs));
    lhkt_test_config_result_count = 0;
    lhkt_test_config_result_pos = 0;
    lhkt_test_config_call_count = 0;
    lhkt_test_write_enabled = 0;
    lhkt_test_write_result = 0;
    lhkt_test_write_call_count = 0;
    bridge_runtime_test_reset_hooks();
}

void lhkt_test_bridge_set_config_results(const int *results, size_t count)
{
    size_t i;

    lhkt_test_config_result_count = 0;
    lhkt_test_config_result_pos = 0;

    if (!results) {
        return;
    }

    if (count > LHKT_TEST_HOOK_MAX_CALLS) {
        count = LHKT_TEST_HOOK_MAX_CALLS;
    }

    for (i = 0; i < count; i++) {
        lhkt_test_config_results[i] = results[i];
    }

    lhkt_test_config_result_count = count;
}

size_t lhkt_test_bridge_config_call_count(void)
{
    return lhkt_test_config_call_count;
}

double lhkt_test_bridge_config_freq_at(size_t index)
{
    if (index >= lhkt_test_config_call_count ||
        index >= LHKT_TEST_HOOK_MAX_CALLS) {
        return 0.0;
    }

    return lhkt_test_config_freqs[index];
}

void lhkt_test_bridge_set_write_result(ssize_t result)
{
    lhkt_test_write_enabled = 1;
    lhkt_test_write_result = result;
}

size_t lhkt_test_bridge_write_call_count(void)
{
    return lhkt_test_write_call_count;
}
#endif

static int bridge_loraham_send_status_request(int conf_fd)
{
    const uint8_t cmd[] = "GET STATUS\n";

    if (conf_fd < 0) {
        return LHKT_ERR;
    }

    if (loraham_sock_write(conf_fd, cmd, sizeof(cmd) - 1) < 0) {
        return LHKT_ERR;
    }

    return LHKT_OK;
}

int bridge_loraham_connect_conf_socket(const lhkt_config_t *cfg,
                                       bridge_conf_state_t *state)
{
    int fd;

    if (!cfg || !state) {
        return LHKT_ERR;
    }

    fd = loraham_sock_connect(cfg->conf_socket);
    if (fd < 0) {
        return LHKT_ERR;
    }

    if (bridge_runtime_set_fd_nonblocking(fd) != LHKT_OK) {
        lhkt_tcp_server_close(fd);
        return LHKT_ERR;
    }

    bridge_conf_state_init(state);

    if (bridge_loraham_send_status_request(fd) != LHKT_OK) {
        lhkt_tcp_server_close(fd);
        return LHKT_ERR;
    }

    printf("[LoRaHAM] CONF socket connected: %s\n", cfg->conf_socket);
    return fd;
}

static int bridge_loraham_send_config_freq(const lhkt_config_t *cfg,
                                           int conf_fd,
                                           double freq)
{
    int ret;

    if (!cfg || freq <= 0.0) {
        return LHKT_ERR_FORMAT;
    }

#ifdef LHKT_TEST
    if (lhkt_test_config_call_count < LHKT_TEST_HOOK_MAX_CALLS) {
        lhkt_test_config_freqs[lhkt_test_config_call_count] = freq;
    }
    lhkt_test_config_call_count++;

    if (lhkt_test_config_result_pos < lhkt_test_config_result_count) {
        return lhkt_test_config_results[lhkt_test_config_result_pos++];
    }

    if (lhkt_test_config_result_count > 0) {
        return LHKT_OK;
    }
#endif

    if (conf_fd < 0) {
        printf("[LoRaHAM] Config socket not connected: %s\n", cfg->conf_socket);
        return LHKT_ERR_CONF_SOCKET;
    }

    ret = loraham_send_config_freq(conf_fd, cfg, freq);
    if (ret != LHKT_OK) {
        printf("[LoRaHAM] Config send failed: freq=%.6f\n", freq);
        return LHKT_ERR_CONF_SOCKET;
    }

    printf("[LoRaHAM] Config sent: freq=%.6f\n", freq);
    return LHKT_OK;
}

int bridge_loraham_send_initial_config(const lhkt_config_t *cfg,
                                       int conf_fd)
{
    int ret;

    if (!cfg) {
        return LHKT_ERR;
    }

#ifdef LHKT_TEST
    if (cfg->have_rx_freq) {
        return bridge_loraham_send_config_freq(cfg, conf_fd, cfg->rx_freq);
    }
#endif

    if (conf_fd < 0) {
        printf("[LoRaHAM] Config socket not connected: %s\n", cfg->conf_socket);
        return LHKT_ERR_CONF_SOCKET;
    }

    ret = loraham_send_config(conf_fd, cfg);
    if (ret != LHKT_OK) {
        printf("[LoRaHAM] Config send failed: %s\n", cfg->conf_socket);
        return LHKT_ERR_CONF_SOCKET;
    }

    printf("[LoRaHAM] Config sent: %s\n", cfg->conf_socket);
    return LHKT_OK;
}

#ifdef LHKT_TEST
int lhkt_test_bridge_send_initial_config(const lhkt_config_t *cfg)
{
    return bridge_loraham_send_initial_config(cfg, -1);
}
#endif

int bridge_loraham_connect_data_socket(const lhkt_config_t *cfg)
{
    int fd;

    if (!cfg) {
        return LHKT_ERR;
    }

    fd = loraham_sock_connect(cfg->data_socket);
    if (fd < 0) {
        return LHKT_ERR;
    }

    printf("[LoRaHAM] Framed data socket connected: %s\n", cfg->data_socket);
    return fd;
}

static ssize_t bridge_loraham_write(int fd,
                                    const uint8_t *buf,
                                    size_t len)
{
#ifdef LHKT_TEST
    lhkt_test_write_call_count++;
    if (lhkt_test_write_enabled) {
        return lhkt_test_write_result;
    }
#endif

    if (loraham_send_framed_tx_packet(fd, buf, len) != LHKT_OK) {
        return -1;
    }

    return (ssize_t)len;
}

static int bridge_loraham_restore_rx_freq(const lhkt_config_t *cfg,
                                          lhkt_stats_t *stats,
                                          int conf_fd)
{
    int ret;

    if (!cfg || !cfg->have_rx_freq) {
        return LHKT_ERR_FORMAT;
    }

    ret = bridge_loraham_send_config_freq(cfg, conf_fd, cfg->rx_freq);
    if (ret == LHKT_OK) {
        return LHKT_OK;
    }

    if (stats) {
        stats->tx_restore_failures++;
    }

    printf("[LoRaHAM] RX freq restore failed, retrying\n");
    if (bridge_runtime_sleep_ms(LHKT_TX_RESTORE_RETRY_DELAY_MS) != LHKT_OK) {
        return ret;
    }

    ret = bridge_loraham_send_config_freq(cfg, conf_fd, cfg->rx_freq);
    if (ret == LHKT_OK) {
        printf("[LoRaHAM] RX freq restore retry OK\n");
        return LHKT_OK;
    }

    if (stats) {
        stats->tx_restore_failures++;
    }

    printf("[LoRaHAM] RX freq restore retry failed\n");
    return ret;
}

int bridge_loraham_should_reconnect_data_socket(int ret)
{
    return ret == LHKT_ERR_TX_SOCKET;
}

int bridge_loraham_should_reconnect_conf_socket(int ret)
{
    return ret == LHKT_ERR_CONF_SOCKET;
}

void bridge_loraham_disconnect_conf_socket(int *conf_fd,
                                           bridge_conf_state_t *conf_state,
                                           lhkt_stats_t *stats,
                                           const char *reason)
{
    if (!conf_fd || *conf_fd < 0) {
        return;
    }

    fprintf(stderr,
            "[WARN] LoRaHAM CONF socket %s, reconnecting\n",
            reason ? reason : "disconnected");

    lhkt_tcp_server_close(*conf_fd);
    *conf_fd = -1;

    if (conf_state) {
        bridge_conf_state_init(conf_state);
    }

    if (stats) {
        stats->socket_reconnects++;
    }
}

void bridge_loraham_disconnect_data_socket(int *data_fd,
                                           loraham_rx_state_t *rx_state,
                                           lhkt_stats_t *stats,
                                           const char *reason)
{
    if (!data_fd || *data_fd < 0) {
        return;
    }

    fprintf(stderr,
            "[WARN] LoRaHAM data socket %s, reconnecting\n",
            reason ? reason : "disconnected");

    lhkt_tcp_server_close(*data_fd);
    *data_fd = -1;

    if (rx_state) {
        loraham_rx_state_init(rx_state);
    }

    if (stats) {
        stats->socket_reconnects++;
    }
}

#ifdef LHKT_TEST
int lhkt_test_bridge_should_reconnect_data_socket(int ret)
{
    return bridge_loraham_should_reconnect_data_socket(ret);
}

int lhkt_test_bridge_should_reconnect_conf_socket(int ret)
{
    return bridge_loraham_should_reconnect_conf_socket(ret);
}

void lhkt_test_bridge_disconnect_data_socket(int *data_fd,
                                             loraham_rx_state_t *rx_state,
                                             lhkt_stats_t *stats)
{
    bridge_loraham_disconnect_data_socket(data_fd, rx_state, stats, "test");
}
#endif

static int bridge_loraham_wait_tx_complete(int conf_fd,
                                           bridge_conf_state_t *conf_state,
                                           const lhkt_config_t *cfg)
{
    long deadline;
    long now;
    int saw_tx;
    int ret;
    unsigned long start_busy_seq;
    unsigned long start_idle_seq;

    if (conf_fd < 0 || !conf_state || !cfg) {
        return LHKT_ERR_UNSUPPORTED;
    }

    start_busy_seq = conf_state->tx_busy_seq;
    start_idle_seq = conf_state->tx_idle_seq;
    saw_tx = conf_state->tx_busy ? 1 : 0;
    deadline = bridge_runtime_now_ms() + LHKT_TX_START_WAIT_MS;

    while (!saw_tx) {
        if (conf_state->tx_busy_seq != start_busy_seq) {
            saw_tx = 1;
            break;
        }

        now = bridge_runtime_now_ms();
        if (now >= deadline) {
            return LHKT_ERR_UNSUPPORTED;
        }

        ret = bridge_conf_wait_read(conf_fd, conf_state, deadline - now);
        if (ret < 0) {
            return LHKT_ERR;
        }
    }

    /*
     * TX=1 and TX=0 may arrive in one read buffer for short packets.
     * The sequence counters preserve that transition even if tx_busy is
     * already back to 0 after parsing the buffer.
     */
    if (!conf_state->tx_busy &&
        conf_state->tx_busy_seq != start_busy_seq &&
        conf_state->tx_idle_seq != start_idle_seq) {
        printf("[CONF] TX complete by event transition\n");
        return LHKT_OK;
    }

    deadline = bridge_runtime_now_ms() + cfg->tx_busy_timeout_ms;
    while (conf_state->tx_busy) {
        if (bridge_runtime_should_stop()) {
            return LHKT_ERR;
        }

        now = bridge_runtime_now_ms();
        if (now >= deadline) {
            printf("[CONF] TX wait timeout\n");
            return LHKT_ERR;
        }

        ret = bridge_conf_wait_read(conf_fd, conf_state, deadline - now);
        if (ret < 0) {
            return LHKT_ERR;
        }
    }

    if (conf_state->tx_idle_seq != start_idle_seq) {
        printf("[CONF] TX complete by idle event\n");
    }

    return LHKT_OK;
}

#ifdef LHKT_TEST
int lhkt_test_bridge_wait_tx_complete_events(const char *events)
{
    int sv[2];
    int ret;
    lhkt_config_t cfg;
    bridge_conf_state_t state;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return LHKT_ERR;
    }

    if (bridge_runtime_set_fd_nonblocking(sv[0]) != LHKT_OK) {
        close(sv[0]);
        close(sv[1]);
        return LHKT_ERR;
    }

    if (events && events[0] != '\0') {
        if (write(sv[1], events, strlen(events)) < 0) {
            close(sv[0]);
            close(sv[1]);
            return LHKT_ERR;
        }
    }

    lhkt_config_defaults(&cfg);
    cfg.tx_busy_timeout_ms = 100;
    bridge_conf_state_init(&state);

    ret = bridge_loraham_wait_tx_complete(sv[0], &state, &cfg);

    close(sv[0]);
    close(sv[1]);
    return ret;
}
#endif

static int bridge_loraham_send_packet(const lhkt_config_t *cfg,
                                      lhkt_stats_t *stats,
                                      int data_fd,
                                      int conf_fd,
                                      bridge_conf_state_t *conf_state,
                                      const uint8_t *packet,
                                      size_t packet_len,
                                      int *packet_written)
{
    int ret;
    int confirm_ret;

    if (packet_written) {
        *packet_written = 0;
    }

    if (!cfg || !packet || packet_len == 0) {
        return LHKT_ERR;
    }

    if (data_fd < 0) {
        if (stats) {
            stats->loraham_drop++;
        }

        printf("[LoRaHAM] TX drop: data socket not connected\n");
        return LHKT_ERR;
    }

    if (!cfg->have_tx_freq || !cfg->have_rx_freq) {
        if (stats) {
            stats->loraham_drop++;
        }

        printf("[LoRaHAM] TX drop: rx_freq and tx_freq required\n");
        return LHKT_ERR_FORMAT;
    }

    ret = bridge_loraham_send_config_freq(cfg, conf_fd, cfg->tx_freq);
    if (ret != LHKT_OK) {
        if (stats) {
            stats->loraham_drop++;
        }

        printf("[LoRaHAM] TX drop: switch to tx_freq failed\n");
        return ret;
    }

    if (bridge_runtime_sleep_ms(cfg->tx_settle_ms) != LHKT_OK) {
        ret = bridge_loraham_restore_rx_freq(cfg, stats, conf_fd);
        if (ret != LHKT_OK) {
            printf("[LoRaHAM] Shutdown during TX settle and RX restore failed\n");
            return ret;
        }

        return LHKT_ERR;
    }

    printf("[LoRaHAM] TX packet len=%zu\n", packet_len);

    if (bridge_loraham_write(data_fd, packet, packet_len) < 0) {
        if (stats) {
            stats->loraham_drop++;
        }

        printf("[LoRaHAM] TX write failed\n");
        (void)bridge_runtime_sleep_ms(cfg->tx_return_ms);
        ret = bridge_loraham_restore_rx_freq(cfg, stats, conf_fd);
        if (ret != LHKT_OK) {
            printf("[LoRaHAM] TX write failed and RX restore failed\n");
        }
        return LHKT_ERR_TX_SOCKET;
    }

    if (packet_written) {
        *packet_written = 1;
    }

    confirm_ret = bridge_loraham_wait_tx_complete(conf_fd, conf_state, cfg);
    if (confirm_ret != LHKT_OK) {
        if (stats) {
            stats->tx_unconfirmed++;
        }

        if (confirm_ret == LHKT_ERR_UNSUPPORTED) {
            printf("[CONF] TX confirmation missing, using fallback wait\n");
        } else {
            printf("[CONF] TX confirmation failed err=%d, using fallback wait\n",
                   confirm_ret);
        }

        (void)bridge_runtime_sleep_ms(cfg->tx_return_ms);
    }

    ret = bridge_loraham_restore_rx_freq(cfg, stats, conf_fd);
    if (ret != LHKT_OK) {
        return ret;
    }

    if (stats) {
        stats->loraham_tx++;
    }

    return LHKT_OK;
}

int bridge_loraham_tx_queue_drain(const lhkt_config_t *cfg,
                                  lhkt_stats_t *stats,
                                  bridge_tx_queue_t *queue,
                                  int data_fd,
                                  int conf_fd,
                                  bridge_conf_state_t *conf_state)
{
    bridge_tx_item_t *item;
    long now;
    int decision;
    int ret;
    int packet_written;

    if (!cfg || !queue) {
        return LHKT_ERR;
    }

    for (;;) {
        item = bridge_tx_queue_head(queue);
        if (!item) {
            return LHKT_OK;
        }

        if (conf_fd >= 0 && conf_state) {
            ret = bridge_conf_read_available(conf_fd, conf_state);
            if (ret != LHKT_OK) {
                /*
                 * The main select loop owns CONF reconnect handling. Keep the
                 * queued packet and let the loop observe/close the socket.
                 */
                return LHKT_OK;
            }
        }

        if (conf_fd >= 0 && conf_state && !conf_state->have_status) {
            return LHKT_OK;
        }

        now = bridge_runtime_now_ms();
        decision = bridge_tx_head_decision(cfg,
                                           conf_fd >= 0 ? conf_state : NULL,
                                           item,
                                           now);
        if (decision == BRIDGE_TX_DECISION_WAIT) {
            return LHKT_OK;
        }

        if (decision == BRIDGE_TX_DECISION_DROP) {
            if (stats) {
                stats->loraham_drop++;
            }

            bridge_tx_queue_pop(queue);
            continue;
        }

        if (data_fd < 0) {
            return LHKT_OK;
        }

        if (conf_fd < 0 || !conf_state) {
            return LHKT_OK;
        }

        packet_written = 0;
        ret = bridge_loraham_send_packet(cfg,
                                         stats,
                                         data_fd,
                                         conf_fd,
                                         conf_state,
                                         item->packet,
                                         item->packet_len,
                                         &packet_written);
        if (packet_written || ret == LHKT_OK) {
            /*
             * Once the framed DATA TX packet was accepted by the daemon
             * socket, never retransmit it because later RX restore failed.
             */
            bridge_tx_queue_pop(queue);
        }

        if (ret != LHKT_OK) {
            return ret;
        }

        return LHKT_OK;
    }
}

#ifdef LHKT_TEST
int lhkt_test_handle_kiss_frame(const kiss_frame_t *kiss_frame,
                                kiss_params_t *kiss_params,
                                const lhkt_config_t *cfg,
                                lhkt_stats_t *stats,
                                int data_fd)
{
    bridge_tx_queue_t queue;
    bridge_conf_state_t conf_state;
    int ret;

    bridge_tx_queue_init(&queue);
    bridge_conf_state_init(&conf_state);

    ret = bridge_kiss_handle_frame(kiss_frame,
                                   kiss_params,
                                   cfg,
                                   stats,
                                   &queue);
    if (ret != LHKT_OK) {
        return ret;
    }

    {
        bridge_tx_item_t *item;
        int packet_written;

        item = bridge_tx_queue_head(&queue);
        if (!item) {
            return LHKT_OK;
        }

        packet_written = 0;
        return bridge_loraham_send_packet(cfg,
                                          stats,
                                          data_fd,
                                          -1,
                                          &conf_state,
                                          item->packet,
                                          item->packet_len,
                                          &packet_written);
    }
}

int lhkt_test_bridge_send_packet_without_conf(uint64_t *tx,
                                              uint64_t *drops,
                                              size_t *write_calls)
{
    lhkt_config_t cfg;
    lhkt_stats_t stats;
    bridge_conf_state_t conf_state;
    int ret;
    int packet_written;
    static const uint8_t packet[] = { 0x3c, 0xff, 0x01, 'T', 'E', 'S', 'T' };

    lhkt_config_defaults(&cfg);
    lhkt_stats_init(&stats);
    bridge_conf_state_init(&conf_state);

    lhkt_test_bridge_reset_tx_hooks();
    lhkt_test_bridge_set_write_result(1);

    packet_written = 0;
    ret = bridge_loraham_send_packet(&cfg,
                                     &stats,
                                     42,
                                     -1,
                                     &conf_state,
                                     packet,
                                     sizeof(packet),
                                     &packet_written);

    if (tx) {
        *tx = stats.loraham_tx;
    }

    if (drops) {
        *drops = stats.loraham_drop;
    }

    if (write_calls) {
        *write_calls = lhkt_test_bridge_write_call_count();
    }

    return ret;
}

int lhkt_test_bridge_send_packet_without_tx_confirm(uint64_t *tx,
                                                     uint64_t *unconfirmed,
                                                     size_t *write_calls)
{
    int sv[2];
    lhkt_config_t cfg;
    lhkt_stats_t stats;
    bridge_conf_state_t conf_state;
    int ret;
    int packet_written;
    int config_results[] = { LHKT_OK, LHKT_OK };
    static const uint8_t packet[] = { 0x3c, 0xff, 0x01, 'T', 'E', 'S', 'T' };

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return LHKT_ERR;
    }

    if (bridge_runtime_set_fd_nonblocking(sv[0]) != LHKT_OK) {
        close(sv[0]);
        close(sv[1]);
        return LHKT_ERR;
    }

    lhkt_config_defaults(&cfg);
    cfg.tx_busy_timeout_ms = 100;
    lhkt_stats_init(&stats);
    bridge_conf_state_init(&conf_state);

    lhkt_test_bridge_reset_tx_hooks();
    lhkt_test_bridge_set_config_results(config_results,
                                        sizeof(config_results) / sizeof(config_results[0]));
    lhkt_test_bridge_set_write_result(1);

    packet_written = 0;
    ret = bridge_loraham_send_packet(&cfg,
                                     &stats,
                                     42,
                                     sv[0],
                                     &conf_state,
                                     packet,
                                     sizeof(packet),
                                     &packet_written);

    if (tx) {
        *tx = stats.loraham_tx;
    }

    if (unconfirmed) {
        *unconfirmed = stats.tx_unconfirmed;
    }

    if (write_calls) {
        *write_calls = lhkt_test_bridge_write_call_count();
    }

    close(sv[0]);
    close(sv[1]);
    return ret;
}

static void lhkt_test_push_dummy_tx_packet(bridge_tx_queue_t *queue,
                                           const lhkt_config_t *cfg)
{
    static const uint8_t packet[] = { 0x3c, 0xff, 0x01, 'T', 'E', 'S', 'T' };

    assert(queue != NULL);
    assert(cfg != NULL);
    assert(bridge_tx_queue_push(queue,
                                cfg,
                                packet,
                                sizeof(packet)) == LHKT_OK);
}

int lhkt_test_bridge_drain_waits_for_data_socket(size_t *queue_depth,
                                                 uint64_t *drops)
{
    lhkt_config_t cfg;
    lhkt_stats_t stats;
    bridge_tx_queue_t queue;
    bridge_conf_state_t conf_state;
    int ret;

    lhkt_config_defaults(&cfg);
    lhkt_stats_init(&stats);
    bridge_tx_queue_init(&queue);
    bridge_conf_state_init(&conf_state);
    lhkt_test_push_dummy_tx_packet(&queue, &cfg);

    ret = bridge_loraham_tx_queue_drain(&cfg, &stats, &queue, -1, -1, &conf_state);

    if (queue_depth) {
        *queue_depth = queue.count;
    }

    if (drops) {
        *drops = stats.loraham_drop;
    }

    return ret;
}

int lhkt_test_bridge_drain_waits_for_conf_socket(size_t *queue_depth,
                                                 size_t *write_calls,
                                                 uint64_t *drops)
{
    lhkt_config_t cfg;
    lhkt_stats_t stats;
    bridge_tx_queue_t queue;
    bridge_conf_state_t conf_state;
    int ret;

    lhkt_config_defaults(&cfg);
    lhkt_stats_init(&stats);
    bridge_tx_queue_init(&queue);
    bridge_conf_state_init(&conf_state);
    lhkt_test_push_dummy_tx_packet(&queue, &cfg);

    lhkt_test_bridge_reset_tx_hooks();
    lhkt_test_bridge_set_write_result(1);

    ret = bridge_loraham_tx_queue_drain(&cfg, &stats, &queue, 42, -1, &conf_state);

    if (queue_depth) {
        *queue_depth = queue.count;
    }

    if (write_calls) {
        *write_calls = lhkt_test_bridge_write_call_count();
    }

    if (drops) {
        *drops = stats.loraham_drop;
    }

    return ret;
}

int lhkt_test_bridge_drain_waits_for_status(size_t *queue_depth,
                                             size_t *write_calls,
                                             int *have_status)
{
    int sv[2];
    lhkt_config_t cfg;
    lhkt_stats_t stats;
    bridge_tx_queue_t queue;
    bridge_conf_state_t conf_state;
    int ret;
    const char events[] = "CAD=0\nTX=0\n";

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return LHKT_ERR;
    }

    if (bridge_runtime_set_fd_nonblocking(sv[0]) != LHKT_OK) {
        close(sv[0]);
        close(sv[1]);
        return LHKT_ERR;
    }

    if (write(sv[1], events, sizeof(events) - 1) < 0) {
        close(sv[0]);
        close(sv[1]);
        return LHKT_ERR;
    }

    lhkt_config_defaults(&cfg);
    lhkt_stats_init(&stats);
    bridge_tx_queue_init(&queue);
    bridge_conf_state_init(&conf_state);
    lhkt_test_push_dummy_tx_packet(&queue, &cfg);

    lhkt_test_bridge_reset_tx_hooks();
    lhkt_test_bridge_set_write_result(1);

    ret = bridge_loraham_tx_queue_drain(&cfg, &stats, &queue, 42, sv[0], &conf_state);

    if (queue_depth) {
        *queue_depth = queue.count;
    }

    if (write_calls) {
        *write_calls = lhkt_test_bridge_write_call_count();
    }

    if (have_status) {
        *have_status = conf_state.have_status;
    }

    close(sv[0]);
    close(sv[1]);
    return ret;
}

int lhkt_test_bridge_drain_reads_pending_conf(size_t *queue_depth,
                                             int *tx_busy,
                                             size_t *write_calls)
{
    int sv[2];
    lhkt_config_t cfg;
    lhkt_stats_t stats;
    bridge_tx_queue_t queue;
    bridge_conf_state_t conf_state;
    int ret;
    const char events[] = "TX=1\n";

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return LHKT_ERR;
    }

    if (bridge_runtime_set_fd_nonblocking(sv[0]) != LHKT_OK) {
        close(sv[0]);
        close(sv[1]);
        return LHKT_ERR;
    }

    if (write(sv[1], events, sizeof(events) - 1) < 0) {
        close(sv[0]);
        close(sv[1]);
        return LHKT_ERR;
    }

    lhkt_config_defaults(&cfg);
    lhkt_stats_init(&stats);
    bridge_tx_queue_init(&queue);
    bridge_conf_state_init(&conf_state);
    lhkt_test_push_dummy_tx_packet(&queue, &cfg);

    lhkt_test_bridge_reset_tx_hooks();
    lhkt_test_bridge_set_write_result(1);

    ret = bridge_loraham_tx_queue_drain(&cfg, &stats, &queue, 42, sv[0], &conf_state);

    if (queue_depth) {
        *queue_depth = queue.count;
    }

    if (tx_busy) {
        *tx_busy = conf_state.tx_busy;
    }

    if (write_calls) {
        *write_calls = lhkt_test_bridge_write_call_count();
    }

    close(sv[0]);
    close(sv[1]);
    return ret;
}

int lhkt_test_bridge_drain_pops_written_restore_failure(size_t *queue_depth,
                                                        uint64_t *restore_failures,
                                                        size_t *write_calls)
{
    int sv[2];
    lhkt_config_t cfg;
    lhkt_stats_t stats;
    bridge_tx_queue_t queue;
    bridge_conf_state_t conf_state;
    int ret;
    int config_results[] = { LHKT_OK, LHKT_ERR, LHKT_ERR };

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return LHKT_ERR;
    }

    if (bridge_runtime_set_fd_nonblocking(sv[0]) != LHKT_OK) {
        close(sv[0]);
        close(sv[1]);
        return LHKT_ERR;
    }

    lhkt_config_defaults(&cfg);
    cfg.tx_busy_timeout_ms = 100;
    lhkt_stats_init(&stats);
    bridge_tx_queue_init(&queue);
    bridge_conf_state_init(&conf_state);
    conf_state.have_status = 1;
    lhkt_test_push_dummy_tx_packet(&queue, &cfg);

    lhkt_test_bridge_reset_tx_hooks();
    lhkt_test_bridge_set_config_results(config_results,
                                        sizeof(config_results) / sizeof(config_results[0]));
    lhkt_test_bridge_set_write_result(1);

    ret = bridge_loraham_tx_queue_drain(&cfg, &stats, &queue, 42, sv[0], &conf_state);

    if (queue_depth) {
        *queue_depth = queue.count;
    }

    if (restore_failures) {
        *restore_failures = stats.tx_restore_failures;
    }

    if (write_calls) {
        *write_calls = lhkt_test_bridge_write_call_count();
    }

    close(sv[0]);
    close(sv[1]);
    return ret;
}
#endif
