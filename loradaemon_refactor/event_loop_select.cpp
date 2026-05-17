#include "event_loop_select.h"

#include <cstddef>

/* --- select() backend --- */

void event_loop_select_reset(EventLoopSelectSet *set)
{
    FD_ZERO(&set->readfds);
    set->maxfd = 0;
}

void event_loop_select_add_fd(EventLoopSelectSet *set, int fd)
{
    if (fd < 0)
        return;

    FD_SET(fd, &set->readfds);

    if (fd >= set->maxfd)
        set->maxfd = fd + 1;
}

int event_loop_select_has_fd(const EventLoopSelectSet *set, int fd)
{
    if (fd < 0)
        return 0;

    return FD_ISSET(fd, &set->readfds) ? 1 : 0;
}

int event_loop_select_fd_limit(const EventLoopSelectSet *set)
{
    return set->maxfd;
}

int event_loop_select_ready_fd(const EventLoopSelectReadySet *ready, int fd)
{
    if (fd < 0)
        return 0;

    return FD_ISSET(fd, &ready->readfds) ? 1 : 0;
}

int event_loop_select_wait(const EventLoopSelectSet *set,
                           EventLoopSelectReadySet *ready,
                           int timeout_usec)
{
    struct timeval tv;

    tv.tv_sec = timeout_usec / 1000000;
    tv.tv_usec = timeout_usec % 1000000;

    ready->readfds = set->readfds;

    return select(set->maxfd, &ready->readfds, NULL, NULL, &tv);
}
