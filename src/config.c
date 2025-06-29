#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // for strcasecmp

// Define the global config instance
server_config_t config;

// Set default configuration values
void load_default_config() {
    config.port = 6379;
    config.concurrency_model = CONCURRENCY_THREADED; // Default to the original model
    config.max_clients = 100;
    strncpy(config.log_file, "crimsoncache.log", sizeof(config.log_file) - 1);
    config.save_after_seconds = 300; // 5 minutes
    config.save_after_changes = 1000;
    config.buffer_size = 1024; // default buffer size
    config.max_events = 64; // default max events for epoll
}

// Simple parser to read key-value pairs from a file
int load_config_from_file(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Warning: Could not open config file");
        printf("Warning: Using default configuration.\n");
        return 0; // Not a fatal error, we can use defaults
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char key[64], value[192];
        if (line[0] == '#' || line[0] == '\n') continue; // Skip comments and empty lines

        if (sscanf(line, "%63s %191[^\n]", key, value) != 2) {
            continue; // Skip malformed lines
        }

        if (strcasecmp(key, "port") == 0) {
            config.port = atoi(value);
        } else if (strcasecmp(key, "concurrency") == 0) {
            if (strcasecmp(value, "eventloop") == 0) {
                config.concurrency_model = CONCURRENCY_EVENTLOOP;
            } else {
                config.concurrency_model = CONCURRENCY_THREADED;
            }
        } else if (strcasecmp(key, "maxClients") == 0) {
            config.max_clients = atoi(value);
        } else if (strcasecmp(key, "logFile") == 0) {
            strncpy(config.log_file, value, sizeof(config.log_file) - 1);
        } else if (strcasecmp(key, "saveSeconds") == 0) {
            config.save_after_seconds = atoi(value);
        } else if (strcasecmp(key, "saveChanges") == 0) {
            config.save_after_changes = atoi(value);
        } else if (strcasecmp(key, "buffer_size") == 0) {
            config.buffer_size = atoi(value);
        } else if (strcasecmp(key, "max_events") == 0) {
            config.max_events = atoi(value);
        }
    }

    fclose(fp);
    return 1;
}
