#include "eventloop.h"
#include "commands.h"
#include "transaction.h" // for tx_init
#include "pubsub.h" // for pubsub_remove_client
#include "config.h" // for config.buffer_size
#include <unistd.h> // for close, read
#include <stdio.h>  // for perror
#include <stdlib.h> // for malloc, free
#include <sys/socket.h> // for accept
#include <netinet/in.h> // for sockaddr_in
#include <string.h> // for memset

extern volatile sig_atomic_t server_running;
extern dict *server_db;
extern client_t *get_client_by_socket(int socket);

// forward declarations for static functions
static void handle_new_connection(event_loop_t *loop, int server_sock);
static void handle_client_message(event_loop_t *loop, int client_sock);

// initialize the event loop
int event_loop_init(event_loop_t *loop) {
    // create a new epoll instance
    loop->epoll_fd = epoll_create1(0);
    if (loop->epoll_fd == -1) {
        perror("epoll_create1 failed");
        return -1;
    }

    // allocate memory for events
    loop->events = malloc(sizeof(struct epoll_event) * config.max_events);
    if (!loop->events) {
        perror("malloc for events failed");
        close(loop->epoll_fd);
        return -1;
    }

    return 0;
}

// clean up the event loop resources
void event_loop_cleanup(event_loop_t *loop) {
    if (loop) {
        if (loop->epoll_fd != -1) {
            close(loop->epoll_fd);
        }
        free(loop->events);
    }
}

// the main event loop
void event_loop_run(event_loop_t *loop, int server_sock) {
    struct epoll_event event;
    event.data.fd = server_sock;
    event.events = EPOLLIN | EPOLLET; // watch for incoming connections, edge-triggered

    // add the server socket to the epoll set
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, server_sock, &event) == -1) {
        perror("epoll_ctl for server_sock failed");
        return;
    }

    printf("server event loop started. waiting for events...\n");

    while (server_running) {
        int num_events = epoll_wait(loop->epoll_fd, loop->events, config.max_events, -1);
        if (num_events == -1) {
            perror("epoll_wait failed");
            continue; // or break, depending on desired error handling
        }

        for (int i = 0; i < num_events; i++) {
            if (loop->events[i].data.fd == server_sock) {
                // event on the listening socket means new connection
                handle_new_connection(loop, server_sock);
            } else {
                // event on a client socket means incoming data
                handle_client_message(loop, loop->events[i].data.fd);
            }
        }
    }
}

// accepts a new connection and adds it to the event loop
static void handle_new_connection(event_loop_t *loop, int server_sock) {
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
    if (client_sock == -1) {
        perror("accept failed");
        return;
    }

    // allocate and initialize client_t
    client_t *client = (client_t *)malloc(sizeof(client_t));
    if (client == NULL) {
        perror("failed to allocate memory for client");
        close(client_sock);
        return;
    }
    client->socket = client_sock;
    memcpy(&client->address, &client_addr, client_len);
    client->addr_len = client_len;
    client->buffer_pos = 0; // initialize buffer position
    client->buffer = (char *)malloc(config.buffer_size);
    if (client->buffer == NULL) {
        perror("failed to allocate client buffer");
        free(client);
        close(client_sock);
        return;
    }
    client->buffer_capacity = config.buffer_size;
    tx_init(client); // initialize transaction state

    // add the new client socket to the epoll set
    struct epoll_event event;
    event.data.fd = client_sock;
    event.events = EPOLLIN | EPOLLET; // watch for input, edge-triggered
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, client_sock, &event) == -1) {
        perror("epoll_ctl for client_sock failed");
        close(client_sock);
        free(client);
        return;
    }

    printf("new client connected on socket %d\n", client_sock);
    register_client(client); // register the client
}

// handles a message from a client
static void handle_client_message(event_loop_t *loop, int client_sock) {
    client_t *client = get_client_by_socket(client_sock);
    if (!client) {
        fprintf(stderr, "error: client not found for socket %d\n", client_sock);
        epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, client_sock, NULL); // remove from epoll
        close(client_sock);
        return;
    }

    int bytes_read = read(client_sock, client->buffer + client->buffer_pos, config.buffer_size - client->buffer_pos - 1);

    if (bytes_read <= 0) {
        // 0 means client closed connection, < 0 is an error
        if (bytes_read < 0) {
            perror("read from client failed");
        }
        printf("client on socket %d disconnected.\n", client_sock);
        epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, client_sock, NULL); // remove from epoll
        pubsub_remove_client(client);
        unregister_client(client);
        close(client_sock);
        free(client->buffer);
        free(client);
    } else {
        client->buffer_pos += bytes_read;
        client->buffer[client->buffer_pos] = '\0';

        // process the command (assuming null-terminated string)
        execute_command(client_sock, client->buffer, server_db);
        client->buffer_pos = 0; // reset buffer for next command
    }
}
