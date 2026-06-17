#define _POSIX_C_SOURCE 200809L
#include "bridge.h"

#include "bridge_conf.h"
#include "bridge_kiss.h"
#include "bridge_loraham.h"
#include "bridge_rx.h"
#include "bridge_runtime.h"
#include "bridge_tx_queue.h"
#include "ax25.h"
#include "kiss.h"
#include "loraham_sock.h"
#include "tcp_server.h"
#include "tnc2.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*
 * Bridge logic.
 * One KISS/TCP client is bridged to one LoRaHAM daemon data socket.
 */

#define LHKT_RX_IDLE_FLUSH_USEC 250000
#define LHKT_RECONNECT_DELAY_SEC 1
#define LHKT_CLIENT_WRITE_TIMEOUT_SEC 2
#define LHKT_TX_RESTORE_RETRY_DELAY_MS 100
#define LHKT_TX_START_WAIT_MS 500
#define LHKT_QUEUE_POLL_USEC 100000
#define LHKT_TEST_HOOK_MAX_CALLS 16

/* ---- Disconnect policy ---- */

static int should_disconnect_kiss_client(int ret)
{
    return ret == LHKT_ERR_CLIENT_SOCKET;
}

#ifdef LHKT_TEST
int lhkt_test_bridge_should_disconnect_kiss_client(int ret)
{
    return should_disconnect_kiss_client(ret);
}
#endif

/* ---- Periodic stats ---- */

static void print_stats_if_due(const lhkt_config_t *cfg,
                               lhkt_stats_t *stats,
                               time_t *next_stats)
{
    time_t now;

    if (!cfg || !stats || !next_stats || cfg->stats_interval <= 0) {
        return;
    }

    now = time(NULL);
    if (*next_stats == 0) {
        *next_stats = now + cfg->stats_interval;
        return;
    }

    if (now < *next_stats) {
        return;
    }

    lhkt_stats_print(stats);
    *next_stats = now + cfg->stats_interval;
}

/* ---- Unit-test entry points ---- */

#ifdef LHKT_TEST
int lhkt_test_send_tnc2_to_kiss_client(int client_fd,
                                       const char *tnc2,
                                       lhkt_stats_t *stats)
{
    bridge_rx_set_should_stop(bridge_runtime_should_stop);
    return bridge_rx_send_tnc2_to_kiss_client(client_fd, tnc2, stats);
}

#endif

/* ---- Main loop ---- */

int lhkt_bridge_run(const lhkt_config_t *cfg, lhkt_stats_t *stats)
{
    int listen_fd;
    int client_fd;
    int data_fd;
    int conf_fd;
    char peer[64];
    uint8_t buf[512];
    ssize_t n;
    size_t i;
    int ret;
    int config_ok;
    int max_fd;
    fd_set rfds;
    struct timeval tv;
    struct timeval *timeout;
    time_t next_stats;
    time_t now;
    long stats_wait;
    unsigned long cad_stats_busy_seq;
    unsigned long cad_stats_idle_seq;

    kiss_decoder_t kiss_dec;
    kiss_frame_t kiss_frame;
    kiss_params_t kiss_params;
    loraham_rx_state_t lora_rx;
    loraham_framed_rx_state_t lora_framed_rx;
    bridge_conf_state_t conf_state;
    bridge_tx_queue_t tx_queue;

    if (!cfg) {
        return 1;
    }

    data_fd = -1;
    conf_fd = -1;
    listen_fd = -1;
    client_fd = -1;
    config_ok = 0;
    next_stats = 0;
    cad_stats_busy_seq = 0;
    cad_stats_idle_seq = 0;

    kiss_decoder_init(&kiss_dec);
    kiss_params_init(&kiss_params);
    loraham_rx_state_init(&lora_rx);
    loraham_framed_rx_state_init(&lora_framed_rx);
    bridge_conf_state_init(&conf_state);
    bridge_tx_queue_init(&tx_queue);

    bridge_runtime_reset_stop_requested();
    bridge_runtime_install_signal_handlers();
    bridge_rx_set_should_stop(bridge_runtime_should_stop);

    conf_fd = bridge_loraham_connect_conf_socket(cfg, &conf_state);
    if (conf_fd < 0) {
        fprintf(stderr,
                "[WARN] LoRaHAM CONF socket not connected yet: %s\n",
                cfg->conf_socket);
    }

    if (bridge_loraham_send_initial_config(cfg, conf_fd) == LHKT_OK) {
        config_ok = 1;
    }

    data_fd = bridge_loraham_connect_data_socket(cfg);
    if (data_fd < 0) {
        fprintf(stderr,
                "[WARN] LoRaHAM data socket not connected yet: %s\n",
                cfg->data_socket);
    }

    if (cfg->rx_only) {
        printf("[LoRaHAM] RX-only mode: TX disabled\n");
    }

    listen_fd = lhkt_tcp_server_listen(cfg->kiss_host, cfg->kiss_port);
    if (listen_fd < 0) {
        fprintf(stderr, "[ERR] KISS/TCP listen failed on %s:%d\n",
                cfg->kiss_host,
                cfg->kiss_port);
        lhkt_tcp_server_close(data_fd);
        lhkt_tcp_server_close(conf_fd);
        return 1;
    }

    printf("[KISS] Listening on %s:%d\n", cfg->kiss_host, cfg->kiss_port);
    printf("[KISS] Waiting for client...\n");

    while (!bridge_runtime_should_stop()) {
        print_stats_if_due(cfg, stats, &next_stats);

        ret = bridge_loraham_tx_queue_drain(cfg,
                                    stats,
                                    &tx_queue,
                                    data_fd,
                                    conf_fd,
                                    &conf_state);
        if (bridge_loraham_should_reconnect_data_socket(ret)) {
            bridge_loraham_disconnect_data_socket(&data_fd,
                                           &lora_rx,
                                           stats,
                                           "TX write failed");
            loraham_framed_rx_state_init(&lora_framed_rx);
            config_ok = 0;
        } else if (bridge_loraham_should_reconnect_conf_socket(ret)) {
            bridge_loraham_disconnect_conf_socket(&conf_fd,
                                           &conf_state,
                                           stats,
                                           "write failed");
            config_ok = 0;
        }

        bridge_conf_collect_cad_stats(&conf_state,
                                      stats,
                                      &cad_stats_busy_seq,
                                      &cad_stats_idle_seq);

        FD_ZERO(&rfds);
        max_fd = -1;

        if (data_fd >= 0) {
            ret = bridge_runtime_add_fd_to_set(data_fd, &rfds, &max_fd);
            if (ret != LHKT_OK) {
                fprintf(stderr, "[ERR] LoRaHAM data fd too large for select\n");
                break;
            }
        }

        if (conf_fd >= 0) {
            ret = bridge_runtime_add_fd_to_set(conf_fd, &rfds, &max_fd);
            if (ret != LHKT_OK) {
                fprintf(stderr, "[ERR] LoRaHAM CONF fd too large for select\n");
                break;
            }
        }

        if (client_fd >= 0) {
            ret = bridge_runtime_add_fd_to_set(client_fd, &rfds, &max_fd);
            if (ret != LHKT_OK) {
                fprintf(stderr, "[ERR] KISS/TCP client fd too large for select\n");
                break;
            }
        } else {
            ret = bridge_runtime_add_fd_to_set(listen_fd, &rfds, &max_fd);
            if (ret != LHKT_OK) {
                fprintf(stderr, "[ERR] KISS/TCP listen fd too large for select\n");
                break;
            }
        }

        timeout = NULL;
        if (data_fd < 0 || conf_fd < 0) {
            tv.tv_sec = LHKT_RECONNECT_DELAY_SEC;
            tv.tv_usec = 0;
            timeout = &tv;
        } else if (!bridge_tx_queue_empty(&tx_queue)) {
            tv.tv_sec = 0;
            tv.tv_usec = LHKT_QUEUE_POLL_USEC;
            timeout = &tv;
        } else if (!config_ok) {
            tv.tv_sec = LHKT_RECONNECT_DELAY_SEC;
            tv.tv_usec = 0;
            timeout = &tv;
        }

        if (stats && cfg->stats_interval > 0) {
            now = time(NULL);
            if (next_stats == 0) {
                next_stats = now + cfg->stats_interval;
            }

            if (next_stats <= now) {
                tv.tv_sec = 0;
                tv.tv_usec = 0;
                timeout = &tv;
            } else {
                stats_wait = (long)(next_stats - now);
                if (!timeout ||
                    tv.tv_sec > stats_wait ||
                    (tv.tv_sec == stats_wait && tv.tv_usec > 0)) {
                    tv.tv_sec = stats_wait;
                    tv.tv_usec = 0;
                    timeout = &tv;
                }
            }
        }

        ret = select(max_fd + 1, &rfds, NULL, NULL, timeout);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "[ERR] select failed\n");
            break;
        }

        if (ret == 0) {
            if (conf_fd < 0) {
                conf_fd = bridge_loraham_connect_conf_socket(cfg, &conf_state);
                continue;
            }

            if (data_fd < 0) {
                data_fd = bridge_loraham_connect_data_socket(cfg);
                if (data_fd >= 0) {
                    if (stats) {
                        stats->socket_reconnects++;
                    }

                    if (bridge_loraham_send_initial_config(cfg, conf_fd) == LHKT_OK) {
                        config_ok = 1;
                    } else {
                        config_ok = 0;
                    }
                }
                continue;
            }

            if (!config_ok) {
                if (bridge_loraham_send_initial_config(cfg, conf_fd) == LHKT_OK) {
                    config_ok = 1;
                }
                continue;
            }

            ret = bridge_rx_handle_loraham_chunk(client_fd,
                                                &lora_rx,
                                                NULL,
                                                0,
                                                stats);
            if (should_disconnect_kiss_client(ret)) {
                printf("[KISS] Client write failed, disconnecting\n");
                lhkt_tcp_server_close(client_fd);
                client_fd = -1;
                kiss_decoder_init(&kiss_dec);
                kiss_params_init(&kiss_params);
                loraham_rx_state_init(&lora_rx);
                loraham_framed_rx_state_init(&lora_framed_rx);
                printf("[KISS] Waiting for client...\n");
            }
            continue;
        }

        if (client_fd < 0 && FD_ISSET(listen_fd, &rfds)) {
            client_fd = lhkt_tcp_server_accept(listen_fd, peer, sizeof(peer));
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }

                fprintf(stderr, "[ERR] KISS/TCP accept failed\n");
                continue;
            }

            if (bridge_runtime_set_fd_nonblocking(client_fd) != LHKT_OK) {
                fprintf(stderr, "[ERR] KISS/TCP non-blocking setup failed\n");
                lhkt_tcp_server_close(client_fd);
                client_fd = -1;
                continue;
            }

            kiss_decoder_init(&kiss_dec);
            kiss_params_init(&kiss_params);
            loraham_rx_state_init(&lora_rx);
            loraham_framed_rx_state_init(&lora_framed_rx);

            printf("[KISS] Client connected: %s\n", peer);
        }

        if (client_fd >= 0 && FD_ISSET(client_fd, &rfds)) {
            n = read(client_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR ||
                    errno == EAGAIN ||
                    errno == EWOULDBLOCK) {
                    continue;
                }

                fprintf(stderr, "[ERR] KISS/TCP read failed\n");
                lhkt_tcp_server_close(client_fd);
                client_fd = -1;
                kiss_decoder_init(&kiss_dec);
                kiss_params_init(&kiss_params);
                loraham_rx_state_init(&lora_rx);
                loraham_framed_rx_state_init(&lora_framed_rx);
                printf("[KISS] Waiting for client...\n");
                continue;
            }

            if (n == 0) {
                printf("[KISS] Client disconnected\n");
                if (stats) {
                    stats->client_disconnects++;
                }

                lhkt_tcp_server_close(client_fd);
                client_fd = -1;
                kiss_decoder_init(&kiss_dec);
                kiss_params_init(&kiss_params);
                loraham_rx_state_init(&lora_rx);
                loraham_framed_rx_state_init(&lora_framed_rx);
                printf("[KISS] Waiting for client...\n");
                continue;
            }

            if (cfg->verbose) {
                printf("[KISS] %zd bytes received\n", n);
            }

            for (i = 0; i < (size_t)n; i++) {
                ret = kiss_decode_byte(&kiss_dec, buf[i], &kiss_frame);

                if (ret == 1) {
                    if (stats) {
                        stats->kiss_rx++;
                    }

                    ret = bridge_kiss_handle_frame(&kiss_frame,
                                                   &kiss_params,
                                                   cfg,
                                                   stats,
                                                   &tx_queue);
                    if (bridge_loraham_should_reconnect_data_socket(ret)) {
                        bridge_loraham_disconnect_data_socket(&data_fd,
                                                       &lora_rx,
                                                       stats,
                                                       "TX write failed");
                        loraham_framed_rx_state_init(&lora_framed_rx);
                        config_ok = 0;
                    } else if (bridge_loraham_should_reconnect_conf_socket(ret)) {
                        bridge_loraham_disconnect_conf_socket(&conf_fd,
                                                       &conf_state,
                                                       stats,
                                                       "write failed");
                        config_ok = 0;
                    }
                } else if (ret < 0) {
                    if (stats) {
                        stats->kiss_drop++;
                    }

                    if (cfg->verbose) {
                        printf("[KISS] Dropped malformed frame: err=%d\n", ret);
                    }
                }
            }
        }

        if (conf_fd >= 0 && FD_ISSET(conf_fd, &rfds)) {
            if (bridge_conf_read_available(conf_fd, &conf_state) != LHKT_OK) {
                bridge_loraham_disconnect_conf_socket(&conf_fd,
                                               &conf_state,
                                               stats,
                                               "disconnected");
                config_ok = 0;
            } else {
                bridge_conf_collect_cad_stats(&conf_state,
                                              stats,
                                              &cad_stats_busy_seq,
                                              &cad_stats_idle_seq);
            }
        }

        if (data_fd >= 0 && FD_ISSET(data_fd, &rfds)) {
            n = loraham_sock_read(data_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }

                bridge_loraham_disconnect_data_socket(&data_fd,
                                               &lora_rx,
                                               stats,
                                               "read failed");
                loraham_framed_rx_state_init(&lora_framed_rx);
                config_ok = 0;
                continue;
            }

            if (n == 0) {
                bridge_loraham_disconnect_data_socket(&data_fd,
                                               &lora_rx,
                                               stats,
                                               "closed");
                loraham_framed_rx_state_init(&lora_framed_rx);
                config_ok = 0;
                continue;
            }

            if (cfg->verbose) {
                printf("[LoRaHAM] %zd bytes received\n", n);
            }

            if (client_fd < 0) {
                loraham_rx_state_init(&lora_rx);
                loraham_framed_rx_state_init(&lora_framed_rx);
                if (stats) {
                    stats->loraham_drop++;
                }

                if (cfg->verbose) {
                    printf("[LoRaHAM] RX drop: no KISS client\n");
                }
            } else {
                ret = bridge_rx_handle_framed_chunk(client_fd,
                                                   &lora_framed_rx,
                                                   buf,
                                                   (size_t)n,
                                                   stats);
                if (should_disconnect_kiss_client(ret)) {
                    printf("[KISS] Client write failed, disconnecting\n");
                    lhkt_tcp_server_close(client_fd);
                    client_fd = -1;
                    kiss_decoder_init(&kiss_dec);
                    kiss_params_init(&kiss_params);
                    loraham_rx_state_init(&lora_rx);
                    printf("[KISS] Waiting for client...\n");
                }
            }
        }
    }

    bridge_conf_collect_cad_stats(&conf_state,
                                  stats,
                                  &cad_stats_busy_seq,
                                  &cad_stats_idle_seq);

    if (bridge_runtime_should_stop()) {
        printf("[Bridge] Shutdown requested\n");
    }

    if (client_fd >= 0) {
        lhkt_tcp_server_close(client_fd);
    }
    lhkt_tcp_server_close(listen_fd);
    lhkt_tcp_server_close(data_fd);
    lhkt_tcp_server_close(conf_fd);

    return 0;
}
