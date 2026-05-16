#include "radio_channel.h"

#include "daemon_protocol.h"
#include "unix_socket.h"

/* --- Radio channel socket/client state --- */

void radio_channel_io_init(RadioChannelIo *ch,
                           RadioBand_t band,
                           const char *data_socket_path,
                           const char *conf_socket_path,
                           int *data_listen_fd,
                           int *conf_listen_fd,
                           int *data_clients,
                           int *conf_clients)
{
    ch->band = band;
    ch->data_socket_path = data_socket_path;
    ch->conf_socket_path = conf_socket_path;
    ch->data_listen_fd = data_listen_fd;
    ch->conf_listen_fd = conf_listen_fd;
    ch->data_clients = data_clients;
    ch->conf_clients = conf_clients;
}

void radio_channel_open_sockets(RadioChannelIo *ch)
{
    *ch->data_listen_fd = setup_unix_socket(ch->data_socket_path, MAX_CLIENTS);
    *ch->conf_listen_fd = setup_unix_socket(ch->conf_socket_path, MAX_CLIENTS);
}
