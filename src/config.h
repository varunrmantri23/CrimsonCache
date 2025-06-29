#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

// Enum for concurrency models
typedef enum {
    CONCURRENCY_THREADED,
    CONCURRENCY_EVENTLOOP
} concurrency_model_t;

// Structure to hold all server configuration
typedef struct server_config {
    int port;
    concurrency_model_t concurrency_model;
    int max_clients;
    char log_file[256];
    int save_after_seconds;
    int save_after_changes;
    int buffer_size;
    int max_events; // max events for epoll
} server_config_t;

// Global server configuration instance
extern server_config_t config;

// Function to load configuration from a file
void load_default_config();
int load_config_from_file(const char *filename);

#endif // CONFIG_H
