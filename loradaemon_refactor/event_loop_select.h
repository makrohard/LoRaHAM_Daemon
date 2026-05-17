#ifndef LORAHAM_EVENT_LOOP_SELECT_H
#define LORAHAM_EVENT_LOOP_SELECT_H

#include <sys/select.h>

/* --- select() backend state --- */

typedef struct {
    fd_set readfds;
    int maxfd;
} EventLoopSelectSet;

typedef struct {
    fd_set readfds;
} EventLoopReadySet;

void event_loop_select_reset(EventLoopSelectSet *set);
void event_loop_select_add_fd(EventLoopSelectSet *set, int fd);
int event_loop_select_has_fd(const EventLoopSelectSet *set, int fd);
int event_loop_select_fd_limit(const EventLoopSelectSet *set);
int event_loop_select_ready_fd(const EventLoopReadySet *ready, int fd);
int event_loop_select_wait(const EventLoopSelectSet *set,
                           EventLoopReadySet *ready,
                           int timeout_usec);

#endif
