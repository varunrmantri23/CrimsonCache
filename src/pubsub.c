#define _POSIX_C_SOURCE 200809L
#include "pubsub.h"
#include "commands.h" // for reply functions
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // for write

pubsub_state_t server_pubsub;

// initialize pub/sub system
void pubsub_init(void) {
    server_pubsub.channels = NULL;
    pthread_mutex_init(&server_pubsub.mutex, NULL);
}

// find or create a channel
static pubsub_channel_t* find_or_create_channel(const char *channel_name) {
    pubsub_channel_t *channel = server_pubsub.channels;
    while (channel) {
        if (strcmp(channel->name, channel_name) == 0) {
            return channel;
        }
        channel = channel->next;
    }

    // channel not found, create it
    channel = malloc(sizeof(pubsub_channel_t));
    if (!channel) return NULL;
    channel->name = strdup(channel_name);
    if (!channel->name) {
        free(channel);
        return NULL;
    }
    channel->subscribers = NULL;
    channel->next = server_pubsub.channels;
    server_pubsub.channels = channel;
    return channel;
}

// subscribe a client to a channel
static int subscribe_to_channel(client_t *client, const char *channel_name) {
    pthread_mutex_lock(&server_pubsub.mutex);
    pubsub_channel_t *channel = find_or_create_channel(channel_name);
    if (!channel) {
        pthread_mutex_unlock(&server_pubsub.mutex);
        return 0; // failed to create channel
    }

    // check if client is already subscribed
    pubsub_client_node_t *node = channel->subscribers;
    while (node) {
        if (node->client == client) {
            pthread_mutex_unlock(&server_pubsub.mutex);
            return 1; // already subscribed
        }
        node = node->next;
    }

    // add client to subscribers list
    pubsub_client_node_t *new_node = malloc(sizeof(pubsub_client_node_t));
    if (!new_node) {
        pthread_mutex_unlock(&server_pubsub.mutex);
        return 0; // out of memory
    }
    new_node->client = client;
    new_node->next = channel->subscribers;
    channel->subscribers = new_node;

    pthread_mutex_unlock(&server_pubsub.mutex);
    return 1;
}

// unsubscribe a client from a specific channel
static int unsubscribe_from_channel(client_t *client, const char *channel_name) {
    pthread_mutex_lock(&server_pubsub.mutex);
    pubsub_channel_t *channel = server_pubsub.channels;
    while (channel) {
        if (strcmp(channel->name, channel_name) == 0) {
            pubsub_client_node_t *curr = channel->subscribers;
            pubsub_client_node_t *prev = NULL;
            while (curr) {
                if (curr->client == client) {
                    if (prev) {
                        prev->next = curr->next;
                    } else {
                        channel->subscribers = curr->next;
                    }
                    free(curr);
                    pthread_mutex_unlock(&server_pubsub.mutex);
                    return 1; // successfully unsubscribed
                }
                prev = curr;
                curr = curr->next;
            }
            break; // channel found, but client not subscribed
        }
        channel = channel->next;
    }
    pthread_mutex_unlock(&server_pubsub.mutex);
    return 0; // channel not found or client not subscribed
}

// subscribe a client to one or more channels
int pubsub_subscribe_client(client_t *client, int argc, char **argv) {
    if (argc < 2) {
        reply_error(client->socket, "err wrong number of arguments for 'subscribe' command");
        return 0;
    }
    int success_count = 0;
    for (int i = 1; i < argc; i++) {
        if (subscribe_to_channel(client, argv[i])) {
            success_count++;
            // send confirmation: "subscribe", channel_name, num_subscriptions
            char resp[256];
            snprintf(resp, sizeof(resp), "*3\r\n$9\r\nsubscribe\r\n$%zu\r\n%s\r\n:%d\r\n",
                     strlen(argv[i]), argv[i], success_count); // this count is per-client, not global
            write(client->socket, resp, strlen(resp));
        } else {
            reply_error(client->socket, "err failed to subscribe to channel");
            // potentially stop or report specific channel failure
        }
    }
    return success_count;
}

// unsubscribe a client from channels
int pubsub_unsubscribe_client(client_t *client, int argc, char **argv) {
    int success_count = 0;
    if (argc == 1) { // unsubscribe from all
        pthread_mutex_lock(&server_pubsub.mutex);
        pubsub_channel_t *channel = server_pubsub.channels;
        while (channel) {
            pubsub_client_node_t *curr = channel->subscribers;
            pubsub_client_node_t *prev = NULL;
            while (curr) {
                if (curr->client == client) {
                    if (prev) {
                        prev->next = curr->next;
                    } else {
                        channel->subscribers = curr->next;
                    }
                    free(curr);
                    success_count++;
                    // send confirmation for this channel
                    char resp[256];
                    snprintf(resp, sizeof(resp), "*3\r\n$11\r\nunsubscribe\r\n$%zu\r\n%s\r\n:%d\r\n",
                             strlen(channel->name), channel->name, 0); // count of remaining subscriptions for this client
                    write(client->socket, resp, strlen(resp));
                    break; // move to next channel
                }
                prev = curr;
                curr = curr->next;
            }
            channel = channel->next;
        }
        pthread_mutex_unlock(&server_pubsub.mutex);
        if (success_count == 0) { // if no channels were unsubscribed (was not subscribed to any)
             char resp[256];
             snprintf(resp, sizeof(resp), "*3\r\n$11\r\nunsubscribe\r\n$-1\r\n:0\r\n"); // nil channel, 0 subscriptions
             write(client->socket, resp, strlen(resp));
        }

    } else { // unsubscribe from specific channels
        for (int i = 1; i < argc; i++) {
            if (unsubscribe_from_channel(client, argv[i])) {
                success_count++;
                 char resp[256];
                 snprintf(resp, sizeof(resp), "*3\r\n$11\r\nunsubscribe\r\n$%zu\r\n%s\r\n:%d\r\n",
                         strlen(argv[i]), argv[i], 0); // count of remaining subscriptions for this client
                 write(client->socket, resp, strlen(resp));
            }
        }
    }
    return success_count;
}

// publish a message to a channel
int pubsub_publish_message(const char *channel_name, const char *message) {
    int receivers = 0;
    pthread_mutex_lock(&server_pubsub.mutex);
    pubsub_channel_t *channel = server_pubsub.channels;
    while (channel) {
        if (strcmp(channel->name, channel_name) == 0) {
            pubsub_client_node_t *node = channel->subscribers;
            while (node) {
                if (node->client) {
                    // format: "message", channel_name, message
                    char resp[1024]; // ensure buffer is large enough
                    snprintf(resp, sizeof(resp), "*3\r\n$7\r\nmessage\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                             strlen(channel_name), channel_name, strlen(message), message);
                    write(node->client->socket, resp, strlen(resp));
                    receivers++;
                }
                node = node->next;
            }
            break;
        }
        channel = channel->next;
    }
    pthread_mutex_unlock(&server_pubsub.mutex);
    return receivers;
}

// remove a client from all subscriptions
void pubsub_remove_client(client_t *client) {
    pthread_mutex_lock(&server_pubsub.mutex);
    pubsub_channel_t *channel = server_pubsub.channels;
    while (channel) {
        pubsub_client_node_t *curr = channel->subscribers;
        pubsub_client_node_t *prev = NULL;
        while (curr) {
            if (curr->client == client) {
                if (prev) {
                    prev->next = curr->next;
                } else {
                    channel->subscribers = curr->next;
                }
                pubsub_client_node_t *to_free = curr;
                curr = curr->next; // advance before freeing
                free(to_free);
                // do not break, client might be subscribed to multiple channels
                // or rather, this loop is per channel, so we found the client in this channel
                // and should continue to the next node in this channel's list
                // but since we are removing this client, we should break from inner loop for this channel
                break; 
            } else {
                prev = curr;
                curr = curr->next;
            }
        }
        channel = channel->next;
    }
    pthread_mutex_unlock(&server_pubsub.mutex);
}

// cleanup pub/sub system
void pubsub_cleanup(void) {
    pthread_mutex_lock(&server_pubsub.mutex);
    pubsub_channel_t *channel = server_pubsub.channels;
    while (channel) {
        pubsub_channel_t *next_channel = channel->next;
        pubsub_client_node_t *node = channel->subscribers;
        while (node) {
            pubsub_client_node_t *next_node = node->next;
            free(node);
            node = next_node;
        }
        free(channel->name);
        free(channel);
        channel = next_channel;
    }
    server_pubsub.channels = NULL;
    pthread_mutex_unlock(&server_pubsub.mutex);
    pthread_mutex_destroy(&server_pubsub.mutex);
}