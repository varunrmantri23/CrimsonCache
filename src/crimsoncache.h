#ifndef CRIMSONCACHE_H
#define CRIMSONCACHE_H

#include <netinet/in.h>
#include <signal.h>
#include "dict.h"
#include "config.h"

// constants
#define DEFAULT_PORT 6379

// client structure
typedef struct client {
    int socket;
    struct sockaddr_storage address; // use sockaddr_storage for ipv4/ipv6 compatibility
    socklen_t addr_len; // store the actual length of the address
    char *buffer;
    size_t buffer_capacity;
    int buffer_pos;
    
    int in_transaction;          // flag to indicate if in MULTI state
    int transaction_errors;      // tracks if any errors occurred during MULTI
    char **queued_commands;     
    int queue_size;              // current size of queue
    int queue_capacity;          // allocated capacity of queue
} client_t;

// function prototypes
void handle_signal(int sig);
void *handle_client(void *arg);
void register_client(client_t *client);
void unregister_client(client_t *client);
client_t *get_client_by_socket(int socket);

// global dictionary
extern dict *server_db;

#endif /* CRIMSONCACHE_H */