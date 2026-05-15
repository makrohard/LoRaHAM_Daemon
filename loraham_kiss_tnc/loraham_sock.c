#include "loraham_sock.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * LoRaHAM daemon sockets are Unix stream sockets.
 * TX writes one complete LoRaHAM APRS packet.
 * RX scans a byte stream for the LoRaHAM APRS header.
 */

void loraham_rx_state_init(loraham_rx_state_t *state)
{
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
}

int loraham_sock_connect(const char *path)
{
    int fd;
    struct sockaddr_un addr;

    if (!path || path[0] == '\0') {
        return LHKT_ERR_FORMAT;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return LHKT_ERR;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        return LHKT_ERR_LONG;
    }

    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return LHKT_ERR;
    }

    return fd;
}

ssize_t loraham_sock_read(int fd,
                          uint8_t *buf,
                          size_t buf_size)
{
    if (fd < 0 || !buf || buf_size == 0) {
        return -1;
    }

    return read(fd, buf, buf_size);
}

ssize_t loraham_sock_write(int fd,
                           const uint8_t *buf,
                           size_t len)
{
    size_t done;
    ssize_t n;

    if (fd < 0 || !buf) {
        return -1;
    }

    done = 0;

    while (done < len) {
        n = write(fd, buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (n == 0) {
            return -1;
        }

        done += (size_t)n;
    }

    return (ssize_t)done;
}

/*
 * Build one LoRaHAM APRS packet:
 *   0x3c 0xff 0x01 + TNC2 text
 *
 * The daemon/RF side must not receive more than 255 bytes.
 */
int loraham_build_aprs_packet(const char *tnc2,
                              uint8_t *out,
                              size_t out_size,
                              size_t *out_len)
{
    size_t len;

    if (!tnc2 || !out || !out_len) {
        return LHKT_ERR;
    }

    len = strlen(tnc2);
    *out_len = 0;

    if (LHKT_LORAHAM_HDR_LEN + len > LHKT_LORAHAM_TX_MAX) {
        return LHKT_ERR_LONG;
    }

    if (out_size < LHKT_LORAHAM_HDR_LEN + len) {
        return LHKT_ERR_NOSPACE;
    }

    out[0] = LORAHAM_APRS_HDR0;
    out[1] = LORAHAM_APRS_HDR1;
    out[2] = LORAHAM_APRS_HDR2;

    memcpy(out + LHKT_LORAHAM_HDR_LEN, tnc2, len);
    *out_len = LHKT_LORAHAM_HDR_LEN + len;

    return LHKT_OK;
}

static int loraham_emit_tnc2(loraham_rx_state_t *state,
                             char *tnc2_out,
                             size_t tnc2_size,
                             size_t *tnc2_len)
{
    if (!state || !tnc2_out || !tnc2_len || tnc2_size == 0) {
        return LHKT_ERR;
    }

    if (state->len == 0) {
        return 0;
    }

    if (state->len >= tnc2_size) {
        loraham_rx_state_init(state);
        *tnc2_len = 0;
        return LHKT_ERR_NOSPACE;
    }

    memcpy(tnc2_out, state->buf, state->len);
    tnc2_out[state->len] = '\0';
    *tnc2_len = state->len;

    loraham_rx_state_init(state);
    return 1;
}

/*
 * Search for 0x3c 0xff 0x01 before accepting text.
 * data_len == 0 flushes a pending packet without CR/LF.
 */
int loraham_extract_tnc2(loraham_rx_state_t *state,
                         const uint8_t *data,
                         size_t data_len,
                         char *tnc2_out,
                         size_t tnc2_size,
                         size_t *tnc2_len)
{
    size_t i;
    uint8_t b;

    if (!state || !tnc2_out || !tnc2_len) {
        return LHKT_ERR;
    }

    *tnc2_len = 0;

    if (data_len == 0) {
        return loraham_emit_tnc2(state, tnc2_out, tnc2_size, tnc2_len);
    }

    if (!data) {
        return LHKT_ERR;
    }

    for (i = 0; i < data_len; i++) {
        b = data[i];

        if (!state->seen_header) {
            if (state->len == 0) {
                if (b == LORAHAM_APRS_HDR0) {
                    state->buf[0] = b;
                    state->len = 1;
                }
                continue;
            }

            if (state->len == 1) {
                if (b == LORAHAM_APRS_HDR1) {
                    state->buf[1] = b;
                    state->len = 2;
                } else if (b == LORAHAM_APRS_HDR0) {
                    state->buf[0] = b;
                    state->len = 1;
                } else {
                    state->len = 0;
                }
                continue;
            }

            if (state->len == 2) {
                if (b == LORAHAM_APRS_HDR2) {
                    state->seen_header = 1;
                    state->len = 0;
                } else if (b == LORAHAM_APRS_HDR0) {
                    state->buf[0] = b;
                    state->len = 1;
                } else {
                    state->len = 0;
                }
                continue;
            }
        }

        if (b == '\r' || b == '\n') {
            return loraham_emit_tnc2(state, tnc2_out, tnc2_size, tnc2_len);
        }

        if (state->len >= sizeof(state->buf) - 1) {
            loraham_rx_state_init(state);
            return LHKT_ERR_LONG;
        }

        state->buf[state->len++] = b;
    }

    return 0;
}

/*
 * Send a minimal LoRa config command.
 * Only explicitly configured frequencies are emitted.
 */
int loraham_send_config(int conf_fd,
                        const lhkt_config_t *cfg)
{
    char line[256];
    int ret;
    double freq;

    if (conf_fd < 0 || !cfg) {
        return LHKT_ERR;
    }

    freq = 0.0;
    if (cfg->have_rx_freq) {
        freq = cfg->rx_freq;
    } else if (cfg->have_tx_freq) {
        freq = cfg->tx_freq;
    }

    if (freq > 0.0) {
        ret = snprintf(line,
                       sizeof(line),
                       "SET MODE=%s FREQ=%.6f SF=%d BW=%.3f CR=%d CRC=%d PREAMBLE=%d SYNC=0x%02X LDRO=%d POWER=%d\n",
                       cfg->mode,
                       freq,
                       cfg->sf,
                       cfg->bw,
                       cfg->cr,
                       cfg->crc,
                       cfg->preamble,
                       cfg->syncword,
                       cfg->ldro,
                       cfg->power);
    } else {
        ret = snprintf(line,
                       sizeof(line),
                       "SET MODE=%s SF=%d BW=%.3f CR=%d CRC=%d PREAMBLE=%d SYNC=0x%02X LDRO=%d POWER=%d\n",
                       cfg->mode,
                       cfg->sf,
                       cfg->bw,
                       cfg->cr,
                       cfg->crc,
                       cfg->preamble,
                       cfg->syncword,
                       cfg->ldro,
                       cfg->power);
    }

    if (ret < 0 || (size_t)ret >= sizeof(line)) {
        return LHKT_ERR_NOSPACE;
    }

    if (loraham_sock_write(conf_fd, (const uint8_t *)line, strlen(line)) < 0) {
        return LHKT_ERR;
    }

    return LHKT_OK;
}
