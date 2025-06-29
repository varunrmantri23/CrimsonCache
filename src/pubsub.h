#ifndef PUBSUB_H
#define PUBSUB_H

#include "crimsoncache.h" // for client_t
#include <pthread.h>

// represents a client subscribed to a channel
typedef struct pubsub_client_node {
    client_t *client;
    struct pubsub_client_node *next;
} pubsub_client_node_t;

// represents a channel and its subscribed clients
typedef struct pubsub_channel {
    char *name;
    pubsub_client_node_t *subscribers;
    struct pubsub_channel *next;
} pubsub_channel_t;

// global pub/sub state
typedef struct {
    pubsub_channel_t *channels;
    pthread_mutex_t mutex;
} pubsub_state_t;

extern pubsub_state_t server_pubsub;

// initialize pub/sub system
void pubsub_init(void);

// subscribe a client to one or more channels
int pubsub_subscribe_client(client_t *client, int argc, char **argv);

// unsubscribe a client from channels
int pubsub_unsubscribe_client(client_t *client, int argc, char **argv);

// publish a message to a channel
int pubsub_publish_message(const char *channel_name, const char *message);

// remove a client from all subscriptions (e.g., on disconnect)
void pubsub_remove_client(client_t *client);

// cleanup pub/sub system
void pubsub_cleanup(void);

#endif /* PUBSUB_H */