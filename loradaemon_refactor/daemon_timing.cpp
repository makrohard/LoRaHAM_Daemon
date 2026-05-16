#include "daemon_timing.h"

/* --- Counter-based tick helper --- */

int daemon_tick_due(int *counter, int interval)
{
    (*counter)++;

    if (*counter >= interval) {
        *counter = 0;
        return 1;
    }

    return 0;
}
