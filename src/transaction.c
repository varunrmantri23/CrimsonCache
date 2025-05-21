#define _POSIX_C_SOURCE 200809L
#include "transaction.h"
#include "commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// initialize transaction state for a client
void tx_init(client_t *client) {
    client->in_transaction = 0;
    client->transaction_errors = 0;
    client->queued_commands = NULL;
    client->queue_size = 0;
    client->queue_capacity = 0;
}

// clean up transaction resources
void tx_cleanup(client_t *client) {
    printf("Cleaning up transaction state for client %p\n", (void*)client);
    
    if (!client) {
        printf("ERROR: Null client in tx_cleanup\n");
        return;
    }
    
    if (client->queued_commands) {
        for (int i = 0; i < client->queue_size; i++) {
            if (client->queued_commands[i]) {
                free(client->queued_commands[i]);
            }
        }
        free(client->queued_commands);
        client->queued_commands = NULL;
    }
    
    // Reset ALL transaction-related fields - CRITICAL
    client->queue_size = 0;
    client->queue_capacity = 0;
    client->in_transaction = 0;      // Most important field to reset
    client->transaction_errors = 0;
    
    printf("Transaction cleanup complete, in_transaction=%d\n", client->in_transaction);
}

// queue a command for later execution
int tx_queue_command(client_t *client, char *cmd) {
    // expand queue if needed
    if (client->queue_size >= client->queue_capacity) {
        int new_capacity = client->queue_capacity == 0 ? 10 : client->queue_capacity * 2;
        char **new_queue = realloc(client->queued_commands, new_capacity * sizeof(char*));
        if (!new_queue) {
            return 0;  // out of memory
        }
        client->queued_commands = new_queue;
        client->queue_capacity = new_capacity;
    }
    
    // add debug output to see the exact command being queued
    printf("Queueing full command: '%s'\n", cmd);
    
    // store the full command
    client->queued_commands[client->queue_size] = strdup(cmd);
    if (!client->queued_commands[client->queue_size]) {
        return 0;  // out of memory
    }
    client->queue_size++;
    return 1;
}

// execute all queued commands
void tx_execute_commands(client_t *client, dict *db) {
    if (client->transaction_errors) {
        reply_error(client->socket, "EXECABORT Transaction discarded because of previous errors");
        tx_cleanup(client);
        return;
    }
    
    int queue_size = client->queue_size;
    printf("Executing transaction with %d commands\n", queue_size);
    
    char **commands = malloc(sizeof(char*) * queue_size);
    if (!commands) {
        reply_error(client->socket, "ERR out of memory during transaction execution");
        tx_cleanup(client);
        return;
    }
    
    for (int i = 0; i < queue_size; i++) {
        commands[i] = strdup(client->queued_commands[i]);
        if (!commands[i]) {
            for (int j = 0; j < i; j++) {
                free(commands[j]);
            }
            free(commands);
            reply_error(client->socket, "ERR out of memory during transaction execution");
            tx_cleanup(client);
            return;
        }
        printf("Command to execute: '%s'\n", commands[i]);
    }
    
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "*%d\r\n", queue_size);
    write(client->socket, buffer, strlen(buffer));
    
    tx_cleanup(client);
    
    for (int i = 0; i < queue_size; i++) {
        printf("Executing full command: '%s'\n", commands[i]);
        execute_command(client->socket, commands[i], db);
        free(commands[i]);
    }
    
    free(commands);
    printf("Transaction execution complete\n");
}

// discard all queued commands
void tx_discard_commands(client_t *client) {
    tx_cleanup(client);
    reply_string(client->socket, "OK");
}