#include "bridge_conf.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

/*
 * Persistent CONF socket state.
 * Tracks daemon status lines and TX/CAD event transitions.
 */

void bridge_conf_state_init(bridge_conf_state_t *state)
{
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
}

static int bridge_parse_flag_line(const char *line,
                                  const char *prefix,
                                  int *out)
{
    size_t len;

    if (!line || !prefix || !out) {
        return 0;
    }

    len = strlen(prefix);
    if (strncmp(line, prefix, len) != 0) {
        return 0;
    }

    if (strcmp(line + len, "1") == 0) {
        *out = 1;
        return 1;
    }

    if (strcmp(line + len, "0") == 0) {
        *out = 0;
        return 1;
    }

    return 0;
}

static int bridge_status_flag_token(const char *line,
                                    const char *key,
                                    int *out)
{
    const char *pos;
    size_t key_len;

    if (!line || !key || !out) {
        return 0;
    }

    pos = strstr(line, key);
    if (!pos) {
        return 0;
    }

    key_len = strlen(key);
    if (pos[key_len] == '1') {
        *out = 1;
        return 1;
    }

    if (pos[key_len] == '0') {
        *out = 0;
        return 1;
    }

    return 0;
}

static void bridge_conf_handle_line(bridge_conf_state_t *state,
                                    const char *line)
{
    int value;

    if (!state || !line || line[0] == '\0') {
        return;
    }

    if (bridge_parse_flag_line(line, "TX=", &value)) {
        state->tx_busy = value;
        state->tx_seq++;
        if (value) {
            state->tx_busy_seq++;
        } else {
            state->tx_idle_seq++;
        }
        printf("[CONF] TX=%d\n", value);
        return;
    }

    if (bridge_parse_flag_line(line, "CAD=", &value)) {
        state->cad_busy = value;
        state->cad_seq++;
        if (value) {
            state->cad_busy_seq++;
        } else {
            state->cad_idle_seq++;
        }
        printf("[CONF] CAD=%d\n", value);
        return;
    }

    if (strncmp(line, "STATUS ", 7) == 0) {
        state->have_status = 1;

        if (strstr(line, "RADIO=READY")) {
            state->radio_ready = 1;
        } else if (strstr(line, "RADIO=FAILED") ||
                   strstr(line, "RADIO=UNINITIALIZED")) {
            state->radio_ready = 0;
        }

        if (bridge_status_flag_token(line, "TX=", &value)) {
            state->tx_busy = value;
        }

        if (bridge_status_flag_token(line, "CAD=", &value)) {
            state->cad_busy = value;
        }

        printf("[CONF] %s\n", line);
        return;
    }
}

void bridge_conf_feed(bridge_conf_state_t *state,
                      const uint8_t *buf,
                      size_t len)
{
    size_t i;
    char c;

    if (!state || (!buf && len > 0)) {
        return;
    }

    for (i = 0; i < len; i++) {
        c = (char)buf[i];

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            state->line[state->line_len] = '\0';
            bridge_conf_handle_line(state, state->line);
            state->line_len = 0;
            continue;
        }

        if (state->line_len + 1 >= sizeof(state->line)) {
            state->line_len = 0;
            continue;
        }

        state->line[state->line_len++] = c;
    }
}


void bridge_conf_collect_cad_stats(const bridge_conf_state_t *state,
                                   lhkt_stats_t *stats,
                                   unsigned long *last_busy_seq,
                                   unsigned long *last_idle_seq)
{
    if (!state || !stats || !last_busy_seq || !last_idle_seq) {
        return;
    }

    if (state->cad_busy_seq < *last_busy_seq) {
        *last_busy_seq = state->cad_busy_seq;
    }

    if (state->cad_idle_seq < *last_idle_seq) {
        *last_idle_seq = state->cad_idle_seq;
    }

    if (state->cad_busy_seq > *last_busy_seq) {
        stats->cad_busy_events += state->cad_busy_seq - *last_busy_seq;
        *last_busy_seq = state->cad_busy_seq;
    }

    if (state->cad_idle_seq > *last_idle_seq) {
        stats->cad_idle_events += state->cad_idle_seq - *last_idle_seq;
        *last_idle_seq = state->cad_idle_seq;
    }

    stats->cad_current_busy = state->cad_busy;
}

int bridge_conf_read_available(int conf_fd,
                               bridge_conf_state_t *state)
{
    uint8_t buf[256];
    ssize_t n;

    if (conf_fd < 0 || !state) {
        return LHKT_ERR;
    }

    for (;;) {
        n = read(conf_fd, buf, sizeof(buf));
        if (n > 0) {
            bridge_conf_feed(state, buf, (size_t)n);
            continue;
        }

        if (n == 0) {
            return LHKT_ERR;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return LHKT_OK;
        }

        return LHKT_ERR;
    }
}

int bridge_conf_wait_read(int conf_fd,
                          bridge_conf_state_t *state,
                          long wait_ms)
{
    fd_set rfds;
    struct timeval tv;
    int ret;

    if (conf_fd < 0 || !state) {
        return LHKT_ERR;
    }

    FD_ZERO(&rfds);
    FD_SET(conf_fd, &rfds);

    if (wait_ms < 0) {
        wait_ms = 0;
    }

    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;

    ret = select(conf_fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR) {
            return 0;
        }

        return LHKT_ERR;
    }

    if (ret == 0) {
        return 0;
    }

    if (bridge_conf_read_available(conf_fd, state) != LHKT_OK) {
        return LHKT_ERR;
    }

    return 1;
}

#ifdef LHKT_TEST
int lhkt_test_bridge_conf_feed_events(const char *text,
                                      int *tx_busy,
                                      int *cad_busy,
                                      int *radio_ready,
                                      int *have_status,
                                      unsigned long *tx_seq,
                                      unsigned long *tx_busy_seq,
                                      unsigned long *tx_idle_seq,
                                      unsigned long *cad_seq,
                                      unsigned long *cad_busy_seq,
                                      unsigned long *cad_idle_seq)
{
    bridge_conf_state_t state;

    bridge_conf_state_init(&state);
    bridge_conf_feed(&state,
                     (const uint8_t *)text,
                     text ? strlen(text) : 0);

    if (tx_busy) {
        *tx_busy = state.tx_busy;
    }

    if (cad_busy) {
        *cad_busy = state.cad_busy;
    }

    if (radio_ready) {
        *radio_ready = state.radio_ready;
    }

    if (have_status) {
        *have_status = state.have_status;
    }

    if (tx_seq) {
        *tx_seq = state.tx_seq;
    }

    if (tx_busy_seq) {
        *tx_busy_seq = state.tx_busy_seq;
    }

    if (tx_idle_seq) {
        *tx_idle_seq = state.tx_idle_seq;
    }

    if (cad_seq) {
        *cad_seq = state.cad_seq;
    }

    if (cad_busy_seq) {
        *cad_busy_seq = state.cad_busy_seq;
    }

    if (cad_idle_seq) {
        *cad_idle_seq = state.cad_idle_seq;
    }

    return LHKT_OK;
}

int lhkt_test_bridge_conf_feed(const char *text,
                               int *tx_busy,
                               int *cad_busy,
                               int *radio_ready,
                               int *have_status)
{
    return lhkt_test_bridge_conf_feed_events(text,
                                             tx_busy,
                                             cad_busy,
                                             radio_ready,
                                             have_status,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL,
                                             NULL);
}
#endif
