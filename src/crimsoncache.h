#ifndef CRIMSONCACHE_H
#define CRIMSONCACHE_H

#include <netinet/in.h>
#include <signal.h>
#include "dict.h"

// constants
#define DEFAULT_PORT 6379
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 100

// client structure
typedef struct client {
    int socket;
    struct sockaddr_in address;
    char buffer[BUFFER_SIZE];
    int buffer_pos;
} client_t;

// function prototypes
void handle_signal(int sig);
void *handle_client(void *arg);

// global dictionary
extern dict *server_db;

#endif /* CRIMSONCACHE_H */