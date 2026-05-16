#ifndef LORAHAM_RADIO_CHANNEL_H
#define LORAHAM_RADIO_CHANNEL_H

/* --- Radio channel identity --- */

typedef enum {
    RADIO_BAND_433 = 433,
    RADIO_BAND_868 = 868
} RadioBand_t;

/* --- Radio modem mode --- */

typedef enum {
    RADIO_MODE_LORA,
    RADIO_MODE_FSK
} RadioMode_t;

/* --- Radio channel socket/client state --- */

typedef struct {
    RadioBand_t band;
    const char *data_socket_path;
    const char *conf_socket_path;
    int *data_listen_fd;
    int *conf_listen_fd;
    int *data_clients;
    int *conf_clients;
} RadioChannelIo;

void radio_channel_io_init(RadioChannelIo *ch,
                           RadioBand_t band,
                           const char *data_socket_path,
                           const char *conf_socket_path,
                           int *data_listen_fd,
                           int *conf_listen_fd,
                           int *data_clients,
                           int *conf_clients);

void radio_channel_open_sockets(RadioChannelIo *ch);

#endif
