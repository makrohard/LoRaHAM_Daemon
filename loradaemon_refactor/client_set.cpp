#include "client_set.h"

#include <string.h>
#include <unistd.h>

/* Preserve original behavior: ignore short writes and dead clients here. */
void client_set_broadcast(int *clients, int max_clients, const char *msg)
{
    size_t len = strlen(msg);

    for (int i = 0; i < max_clients; i++) {
        if (clients[i] > 0)
            write(clients[i], msg, len);
    }
}
