#ifndef LORAHAM_CONFIG_STATUS_H
#define LORAHAM_CONFIG_STATUS_H

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "radio_controller.h"
#include "radio_health.h"

/* --- CONF status query --------------------------------------------------- */

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

static inline int config_status_is_get_status(const char *line)
{
    char cmd[64];

    config_status_trim_copy(cmd, sizeof(cmd), line);
    config_status_uppercase(cmd);

    return strcmp(cmd, "GET STATUS") == 0;
}

template<typename RadioT>
static inline void config_status_format(char *buf,
                                        size_t buf_size,
                                        const RadioController<RadioT> *ctrl)
{
    snprintf(buf,
             buf_size,
             "STATUS RADIO=%s TX=%d CAD=%d GETRSSI=%d\n",
             radio_health_name(radio_controller_health(ctrl)),
             (ctrl && ctrl->tx_busy) ? 1 : 0,
             (ctrl && ctrl->cad_active) ? 1 : 0,
             (ctrl && ctrl->getrssi_active) ? 1 : 0);
}

#endif
