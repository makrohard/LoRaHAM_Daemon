#ifndef LORAHAM_DAEMON_PROTOCOL_H
#define LORAHAM_DAEMON_PROTOCOL_H

/* --- Public daemon limits --- */

#define buf_SIZE 256
#define MAX_CLIENTS 10

/* --- CAD monitoring cadence --- */

#define DAEMON_CAD_POLL_INTERVAL_MS 200

/* --- Public Unix socket paths --- */

#define DATA868_SOCKET "/tmp/lora868.sock"
#define DATA433_SOCKET "/tmp/lora433.sock"
#define DATA868_FRAMED_SOCKET "/tmp/lora868f.sock"
#define DATA433_FRAMED_SOCKET "/tmp/lora433f.sock"
#define CONF868_SOCKET "/tmp/loraconf868.sock"
#define CONF433_SOCKET "/tmp/loraconf433.sock"

/* --- Test aliases --- */

#define SOCK_DATA_433 DATA433_SOCKET
#define SOCK_DATA_868 DATA868_SOCKET
#define SOCK_DATA_FRAMED_433 DATA433_FRAMED_SOCKET
#define SOCK_DATA_FRAMED_868 DATA868_FRAMED_SOCKET
#define SOCK_CONF_433 CONF433_SOCKET
#define SOCK_CONF_868 CONF868_SOCKET

#endif
