#ifndef LORAHAM_CLIENT_SET_H
#define LORAHAM_CLIENT_SET_H

/* Broadcast text to the currently connected clients. */
void client_set_broadcast(int *clients, int max_clients, const char *msg);

#endif
