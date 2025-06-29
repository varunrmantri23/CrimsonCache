#ifndef EVENTLOOP_H
#define EVENTLOOP_H

#include <sys/epoll.h>
#include "crimsoncache.h"
#include "config.h"

// represents the state of our event loop
typedef struct event_loop {
    int epoll_fd; // file descriptor for the epoll instance
    struct epoll_event *events; // array to hold triggered events
} event_loop_t;

// Functions to manage the event loop
int event_loop_init(event_loop_t *loop);
void event_loop_run(event_loop_t *loop, int server_sock);
void event_loop_cleanup(event_loop_t *loop);

#endif // EVENTLOOP_H
