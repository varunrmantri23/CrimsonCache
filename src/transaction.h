#ifndef TRANSACTION_H
#define TRANSACTION_H

#include "crimsoncache.h"
#include "dict.h"

void tx_init(client_t *client);


void tx_cleanup(client_t *client);


int tx_queue_command(client_t *client, char *cmd);


void tx_execute_commands(client_t *client, dict *db);


void tx_discard_commands(client_t *client);

#endif /* TRANSACTION_H */