#ifndef CRIMSONCACHE_H
#define CRIMSONCACHE_H

#include <netinet/in.h>
#include <signal.h>

// Constants
#define DEFAULT_PORT 6379
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 100

// Client structure
typedef struct {
    int socket; //sokcet file descriptor
    struct sockaddr_in address;
} client_t;



// Function prototypes
void handle_signal(int sig);
void *handle_client(void *arg);

#endif 