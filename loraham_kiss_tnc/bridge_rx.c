#include "bridge_rx.h"

#include "ax25.h"
#include "kiss.h"
#include "tnc2.h"

#include <errno.h>
#include <stdio.h>
#include <sys/select.h>
#include <unistd.h>

/*
 * LoRaHAM RX path.
 * Converts daemon RX packets to KISS frames for the connected client.
 */

#define BRIDGE_RX_CLIENT_WRITE_TIMEOUT_SEC 2
#define BRIDGE_RX_LORAHAM_META_LEN 4

static bridge_rx_should_stop_fn bridge_rx_should_stop_cb;

void bridge_rx_set_should_stop(bridge_rx_should_stop_fn should_stop)
{
    bridge_rx_should_stop_cb = should_stop;
}

static int bridge_rx_should_stop(void)
{
    if (!bridge_rx_should_stop_cb) {
        return 0;
    }

    return bridge_rx_should_stop_cb() != 0;
}

static int bridge_rx_wait_fd_writable(int fd)
{
    fd_set wfds;
    struct timeval tv;
    int ret;

    if (fd < 0 || fd >= FD_SETSIZE) {
        return LHKT_ERR;
    }

    for (;;) {
        if (bridge_rx_should_stop()) {
            return LHKT_ERR;
        }

        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);

        tv.tv_sec = BRIDGE_RX_CLIENT_WRITE_TIMEOUT_SEC;
        tv.tv_usec = 0;

        ret = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                if (bridge_rx_should_stop()) {
                    return LHKT_ERR;
                }

                continue;
            }

            return LHKT_ERR;
        }

        if (ret == 0) {
            errno = ETIMEDOUT;
            return LHKT_ERR;
        }

        return LHKT_OK;
    }
}

#ifdef LHKT_TEST
int bridge_rx_test_wait_fd_writable(int fd)
{
    return bridge_rx_wait_fd_writable(fd);
}
#endif

static int bridge_rx_write_all_fd(int fd, const uint8_t *buf, size_t len)
{
    size_t done;
    ssize_t n;

    if (fd < 0 || fd >= FD_SETSIZE || (!buf && len > 0)) {
        return LHKT_ERR;
    }

    done = 0;

    while (done < len) {
        n = write(fd, buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (bridge_rx_wait_fd_writable(fd) != LHKT_OK) {
                    return LHKT_ERR;
                }

                continue;
            }

            return LHKT_ERR;
        }

        if (n == 0) {
            return LHKT_ERR;
        }

        done += (size_t)n;
    }

    return LHKT_OK;
}

int bridge_rx_send_tnc2_to_kiss_client(int client_fd,
                                       const char *tnc2,
                                       lhkt_stats_t *stats)
{
    ax25_frame_t ax25;
    uint8_t ax25_raw[LHKT_AX25_MAX_FRAME];
    uint8_t kiss_raw[LHKT_KISS_MAX_FRAME];
    size_t ax25_len;
    size_t kiss_len;
    int ret;

    if (client_fd < 0 || !tnc2) {
        return LHKT_ERR;
    }

    printf("[TNC2 RX] %s\n", tnc2);

    ret = tnc2_parse_line(tnc2, &ax25);
    if (ret != LHKT_OK) {
        if (stats) {
            stats->tnc2_drop++;
        }

        printf("[TNC2] RX drop, parse failed: err=%d\n", ret);
        return ret;
    }

    if (stats) {
        stats->tnc2_rx++;
    }

    ret = ax25_encode_ui(&ax25, ax25_raw, sizeof(ax25_raw), &ax25_len);
    if (ret != LHKT_OK) {
        if (stats) {
            stats->ax25_drop++;
        }

        printf("[AX25] RX drop, encode failed: err=%d\n", ret);
        return ret;
    }

    if (stats) {
        stats->ax25_tx++;
    }

    ret = kiss_encode_data_frame(ax25_raw,
                                 ax25_len,
                                 kiss_raw,
                                 sizeof(kiss_raw),
                                 &kiss_len);
    if (ret != LHKT_OK) {
        if (stats) {
            stats->kiss_drop++;
        }

        printf("[KISS] RX drop, encode failed: err=%d\n", ret);
        return ret;
    }

    if (bridge_rx_write_all_fd(client_fd, kiss_raw, kiss_len) != LHKT_OK) {
        if (stats) {
            stats->kiss_drop++;
        }

        printf("[KISS] Client write failed\n");
        return LHKT_ERR_CLIENT_SOCKET;
    }

    if (stats) {
        stats->kiss_tx++;
    }

    printf("[KISS] Sent RX frame to client len=%zu\n", kiss_len);

    return LHKT_OK;
}

int bridge_rx_handle_loraham_chunk(int client_fd,
                                   loraham_rx_state_t *rx_state,
                                   const uint8_t *buf,
                                   size_t len,
                                   lhkt_stats_t *stats)
{
    char tnc2[LHKT_TNC2_MAX_LINE];
    size_t tnc2_len;
    int ret;

    for (;;) {
        ret = loraham_extract_tnc2(rx_state,
                                   buf,
                                   len,
                                   tnc2,
                                   sizeof(tnc2),
                                   &tnc2_len);
        if (ret == 1) {
            if (stats) {
                stats->loraham_rx++;
            }

            ret = bridge_rx_send_tnc2_to_kiss_client(client_fd, tnc2, stats);
            if (ret != LHKT_OK) {
                return ret;
            }
        }

        if (ret < 0) {
            if (stats) {
                stats->loraham_drop++;
            }

            printf("[LoRaHAM] RX drop: err=%d\n", ret);
            return ret;
        }

        /* Drain queued complete packets without forcing tail flush. */
        if (rx_state->pending_len == 0) {
            break;
        }

        buf = NULL;
        len = 0;
    }

    return LHKT_OK;
}

static int bridge_rx_send_loraham_packet_to_kiss_client(int client_fd,
                                                        const uint8_t *packet,
                                                        size_t packet_len,
                                                        lhkt_stats_t *stats)
{
    loraham_rx_state_t one_packet;
    uint64_t before;
    int ret;

    if (client_fd < 0 || (!packet && packet_len > 0)) {
        return LHKT_ERR;
    }

    before = stats ? stats->loraham_rx : 0;
    loraham_rx_state_init(&one_packet);

    ret = bridge_rx_handle_loraham_chunk(client_fd,
                                         &one_packet,
                                         packet,
                                         packet_len,
                                         stats);
    if (ret != LHKT_OK) {
        return ret;
    }

    ret = bridge_rx_handle_loraham_chunk(client_fd,
                                         &one_packet,
                                         NULL,
                                         0,
                                         stats);
    if (ret != LHKT_OK) {
        return ret;
    }

    if (stats && stats->loraham_rx == before) {
        stats->loraham_drop++;
        printf("[LoRaHAM] Framed RX drop: no APRS packet\n");
        return LHKT_ERR_FORMAT;
    }

    return LHKT_OK;
}

int bridge_rx_handle_framed_chunk(int client_fd,
                                  loraham_framed_rx_state_t *frame_state,
                                  const uint8_t *buf,
                                  size_t len,
                                  lhkt_stats_t *stats)
{
    loraham_frame_t frame;
    size_t i;
    int ret;

    if (!frame_state || (!buf && len > 0)) {
        return LHKT_ERR;
    }

    for (i = 0; i < len; i++) {
        ret = loraham_framed_decode_byte(frame_state, buf[i], &frame);

        if (ret == 1) {
            if (frame.type == LORAHAM_FRAME_RX_PACKET) {
                if (frame.payload_len < BRIDGE_RX_LORAHAM_META_LEN) {
                    if (stats) {
                        stats->loraham_drop++;
                    }

                    printf("[LoRaHAM] Framed RX drop: short metadata len=%zu\n",
                           frame.payload_len);
                    continue;
                }

                ret = bridge_rx_send_loraham_packet_to_kiss_client(
                    client_fd,
                    frame.payload + BRIDGE_RX_LORAHAM_META_LEN,
                    frame.payload_len - BRIDGE_RX_LORAHAM_META_LEN,
                    stats);
                if (ret != LHKT_OK) {
                    return ret;
                }
            } else if (frame.type == LORAHAM_FRAME_ERROR) {
                if (stats) {
                    stats->loraham_framed_errors++;
                }

                printf("[LoRaHAM] Framed ERROR: %.*s\n",
                       (int)frame.payload_len,
                       (const char *)frame.payload);
            } else {
                if (stats) {
                    stats->loraham_drop++;
                }

                printf("[LoRaHAM] Framed RX drop: unsupported type=0x%02X\n",
                       frame.type);
            }
        } else if (ret < 0) {
            if (stats) {
                stats->loraham_drop++;
            }

            printf("[LoRaHAM] Framed RX drop: err=%d\n", ret);
            return ret;
        }
    }

    return LHKT_OK;
}
