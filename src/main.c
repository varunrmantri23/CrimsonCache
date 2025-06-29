#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> //for system calls
#include <signal.h>
#include <netinet/in.h> //internet address structs
#include <sys/socket.h> // socket functions (socket, bind, listen, accept)
#include <arpa/inet.h> 
#include <pthread.h> // POSIX threads for concurrency 
#include <time.h> // for time-related functions
#include <errno.h>
#include <strings.h>  // For strcasecmp
#include "crimsoncache.h"
#include "commands.h"
#include "persistence.h"
#include "replication.h"
#include "transaction.h"
#include "pubsub.h"
#include "config.h"
#include "eventloop.h"

// max_clients is now configured via crimsoncache.conf
client_t **client_list;

// global variables for persistence, now configured via config.h
struct {
    int changes_since_save;
    time_t last_save;
} server_persistence;

void register_client(client_t *client) {
    for (int i = 0; i < config.max_clients; i++) {
        if (client_list[i] == NULL) {
            client_list[i] = client;
            // tx_init(client); // Initialized when client_t is created
            break;
        }
    }
}

void unregister_client(client_t *client) {
    for (int i = 0; i < config.max_clients; i++) {
        if (client_list[i] == client) {
            tx_cleanup(client); // Clean up transaction resources
            client_list[i] = NULL;
            break;
        }
    }
}

client_t *get_client_by_socket(int socket) {
    for (int i = 0; i < config.max_clients; i++) {
        if (client_list[i] && client_list[i]->socket == socket) {
            return client_list[i];
        }
    }
    return NULL;
}

// background thread function prototypes
void *cleanup_expired_keys(void *arg);
void *auto_save_thread(void *arg);
void *replication_thread(void *arg);
void track_command_change(void);

// global dictionary (server database)
dict *server_db = NULL;

volatile sig_atomic_t server_running = 1;
void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\nshutting down server...\n");
        server_running = 0;
    }
}
// thread for cleaning expired keys from the database
void *cleanup_expired_keys(void *arg) {
    (void)arg; // unused parameter
    
    while (server_running) {
        dict_clear_expired(server_db);
        sleep(1); // check every second
    }
    
    return NULL;
}

// thread for auto-saving the database
void *auto_save_thread(void *arg) {
    (void)arg; // unused parameter
    
    while (server_running) {
        sleep(1); // check every second
        
        time_t now = time(NULL);
        int time_since_save = now - server_persistence.last_save;

        if ((config.save_after_changes > 0 && server_persistence.changes_since_save >= config.save_after_changes) ||
            (config.save_after_seconds > 0 && time_since_save >= config.save_after_seconds && server_persistence.changes_since_save > 0)) {
            
            printf("auto-saving the database after %d changes and %d seconds...\n", 
                   server_persistence.changes_since_save, time_since_save);
            
            if (save_rdb_to_file(server_db, "dump.rdb")) {
                server_persistence.changes_since_save = 0;
                server_persistence.last_save = now;
            }
        }
    }
    
    return NULL;
}

// thread for handling replication tasks
void *replication_thread(void *arg) {
    (void)arg;
    
    char buffer[config.buffer_size * 4]; // buffer for incoming data
    int buffer_pos = 0;
    
    while (server_running) {
        // handle replica duties
        if (server_repl.role == ROLE_REPLICA) {
            // process data from primary if in connected state
            if (server_repl.state == REPL_STATE_CONNECTED || 
                server_repl.state == REPL_STATE_SYNC) {
                
                int bytes_read = recv(server_repl.primary_fd, buffer + buffer_pos, 
                                     config.buffer_size - buffer_pos - 1, 0);
                
                if (bytes_read > 0) {
                    buffer_pos += bytes_read;
                    buffer[buffer_pos] = '\0';
                    
                    // Process buffer line by line
                    char *line = buffer;
                    char *next_line;
                    
                    while ((next_line = strstr(line, "\r\n")) != NULL) {
                        *next_line = '\0'; // Terminate the current line
                        
                        // Skip empty lines
                        if (strlen(line) > 0) {
                            // Log the command we're about to execute
                            printf("Replica executing: %s\n", line);
                            
                            // Process this command
                            execute_command(-1, line, server_db);
                        }
                        
                        // Move to the next line (skip \r\n)
                        line = next_line + 2;
                        
                        // Update offset
                        server_repl.repl_offset += (next_line + 2 - buffer);
                    }
                    
                    // Move any incomplete command to the beginning of the buffer
                    if (line < buffer + buffer_pos) {
                        buffer_pos = buffer + buffer_pos - line;
                        memmove(buffer, line, buffer_pos);
                    } else {
                        buffer_pos = 0;
                    }
                    
                    // If we were in SYNC state, move to CONNECTED
                    if (server_repl.state == REPL_STATE_SYNC) {
                        server_repl.state = REPL_STATE_CONNECTED;
                        printf("replication: connected to primary\n");
                    }
                } 
                else if (bytes_read == 0 || 
                        (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    // Connection closed or error
                    printf("replication: connection to primary lost\n");
                    close(server_repl.primary_fd);
                    server_repl.primary_fd = -1;
                    server_repl.state = REPL_STATE_CONNECTING;
                    
                    // Try to reconnect after a delay
                    sleep(1);
                    replication_set_primary(server_repl.primary_host, server_repl.primary_port);
                }
            }
            // if in connecting state, attempt to connect
            else if (server_repl.state == REPL_STATE_CONNECTING) {
                if (server_repl.primary_fd == -1) {
                    replication_set_primary(server_repl.primary_host, server_repl.primary_port);
                }
                sleep(1); // avoid busy waiting
            }
        }
        
        // sleep to avoid busy waiting
        struct timespec ts = {0, 100000000}; // 100ms
        nanosleep(&ts, NULL);
    }
    
    return NULL;
}

// track changes for persistence
void track_command_change(void) {
    server_persistence.changes_since_save++;
}

// handles client connections in threaded model
void *handle_client(void *arg) {
    client_t *client = (client_t *)arg;
    int client_sock = client->socket;
    char client_ip_str[INET6_ADDRSTRLEN];
    
    if (client->address.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&client->address;
        inet_ntop(AF_INET, &s->sin_addr, client_ip_str, sizeof(client_ip_str));
        printf("new client connected: %s:%d\n", 
               client_ip_str, ntohs(s->sin_port));
    } else if (client->address.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client->address;
        inet_ntop(AF_INET6, &s->sin6_addr, client_ip_str, sizeof(client_ip_str));
        printf("new client connected: %s:%d\n", 
               client_ip_str, ntohs(s->sin6_port));
    } else {
        printf("new client connected: unknown address family\n");
    }
    
    register_client(client);
    
    while (server_running) {
        int bytes_read = recv(client_sock, client->buffer + client->buffer_pos, client->buffer_capacity - client->buffer_pos - 1, 0);
        if (bytes_read <= 0) {
            printf("client %s:%d disconnected\n", 
                   client_ip_str, 
                   (client->address.ss_family == AF_INET) ? ntohs(((struct sockaddr_in *)&client->address)->sin_port) : ntohs(((struct sockaddr_in6 *)&client->address)->sin6_port));
            break;
        }
        
        client->buffer_pos += bytes_read;
        client->buffer[client->buffer_pos] = '\0';
        
        execute_command(client_sock, client->buffer, server_db);
        client->buffer_pos = 0; // reset buffer for next command
    }
    pubsub_remove_client(client);
    unregister_client(client);
    close(client_sock);
    free(client->buffer);
    free(client);
    return NULL;
}

// function to run threaded server with custom port
void run_threaded_server(int port) {
    int server_sock, client_sock;
    struct sockaddr_in6 server_addr;
    struct sockaddr_storage client_addr; // Use sockaddr_storage for client_addr - handles both IPv4 and IPv6 well
    socklen_t client_len = sizeof(client_addr);
    pthread_t thread_id;
    
    // now using the provided port parameter
    printf("CrimsonCache starting on port %d using threaded model\n", port);
    
    // set up signal handling
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    // create dual-stack socket (IPv6 that also accepts IPv4)
    if ((server_sock = socket(AF_INET6, SOCK_STREAM, 0)) < 0) {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }

    // set SO_REUSEADDR to allow rebinding to the port
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // allow IPv4 connections on IPv6 socket (disable IPV6_V6ONLY)
    int ipv6_only = 0;
    if (setsockopt(server_sock, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_only, sizeof(ipv6_only)) < 0) {
        perror("Warning: Could not set IPV6_V6ONLY option");
    }

    // set up server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(port);
    
    // bind socket
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    
    // listen for connections
    if (listen(server_sock, config.max_clients) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    
    printf("Server is ready to accept connections\n");
    
    // start background thread for key expiration
    pthread_t cleanup_thread;
    if (pthread_create(&cleanup_thread, NULL, cleanup_expired_keys, NULL) != 0) {
        perror("Failed to create cleanup thread");
    }
    
    // start auto-save thread
    pthread_t autosave_thread;
    if (pthread_create(&autosave_thread, NULL, auto_save_thread, NULL) != 0) {
        perror("failed to create auto-save thread");
    }
    
    // Start replication thread
    pthread_t repl_thread;
    if (pthread_create(&repl_thread, NULL, replication_thread, NULL) != 0) {
        perror("failed to create replication thread");
        // Non-fatal, continue
    }
    
    // accept connections and handle clients
    while (server_running) {
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            if (server_running) perror("Accept failed");
            continue;
        }
        
        client_t *client = (client_t *)malloc(sizeof(client_t));
        if (client == NULL) {
            perror("failed to allocate memory for client");
            close(client_sock);
            continue;
        }
        
        client->socket = client_sock;
        // copy client address information
        memcpy(&client->address, &client_addr, client_len);
        client->addr_len = client_len;
        client->buffer_pos = 0; // initialize buffer position
        client->buffer = (char *)malloc(config.buffer_size);
        if (client->buffer == NULL) {
            perror("failed to allocate client buffer");
            free(client);
            close(client_sock);
            continue;
        }
        client->buffer_capacity = config.buffer_size;
        tx_init(client); // initialize transaction state
        
        if (pthread_create(&thread_id, NULL, handle_client, (void *)client) != 0) {
            perror("Failed to create thread");
            free(client->buffer);
            free(client);
            close(client_sock);
        } else {
            pthread_detach(thread_id);
        }
    }
    
    //clean
    close(server_sock);
    printf("Server shutdown complete\n");
}


// run the server using the event loop model
void run_eventloop_server(int port) {
    int server_sock;
    struct sockaddr_in6 server_addr;
    event_loop_t loop;

    printf("CrimsonCache starting on port %d using eventloop model\n", port);

    // Basic server setup (socket, bind, listen)
    if ((server_sock = socket(AF_INET6, SOCK_STREAM, 0)) < 0) {
        perror("Failed to create socket");
        return;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int ipv6_only = 0;
    setsockopt(server_sock, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_only, sizeof(ipv6_only));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(port);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_sock);
        return;
    }

    if (listen(server_sock, config.max_clients) < 0) {
        perror("Listen failed");
        close(server_sock);
        return;
    }

    // Initialize and run the event loop
    if (event_loop_init(&loop) != 0) {
        fprintf(stderr, "Failed to initialize event loop\n");
        close(server_sock);
        return;
    }

    // The background threads are still managed by main
    event_loop_run(&loop, server_sock);

    // Cleanup
    event_loop_cleanup(&loop);
    close(server_sock);
    printf("Server shutdown complete\n");
}

int main(int argc, char *argv[]) {
    // load default configuration
    load_default_config();

    // handle command line arguments
    if (argc > 1) {
        // check if the argument is a port number
        char *endptr;
        long arg_port = strtol(argv[1], &endptr, 10);
        if (*endptr == '\0' && arg_port > 0 && arg_port <= 65535) {
            // it's a valid port number, override default port
            config.port = (int)arg_port;
            fprintf(stderr, "info: starting on port %d as specified by command line argument.\n", config.port);
        } else {
            // it's not a port number, treat it as a config file path
            const char *config_file = argv[1];
            // check if config file exists
            if (access(config_file, F_OK) == -1) {
                fprintf(stderr, "warning: config file '%s' not found. using default settings.\n", config_file);
                fprintf(stderr, "you can create a '%s' file to customize settings.\n", config_file);
            } else {
                load_config_from_file(config_file);
            }
        }
    } else {
        // no command line arguments, try to load default config file
        const char *default_config_file = "crimsoncache.conf";
        if (access(default_config_file, F_OK) == -1) {
            fprintf(stderr, "warning: config file '%s' not found. using default settings.\n", default_config_file);
            fprintf(stderr, "you can create a '%s' file to customize settings.\n", default_config_file);
        } else {
            load_config_from_file(default_config_file);
        }
    }

    // initialize server database


    server_db = dict_create(1024);
    if (!server_db) {
        fprintf(stderr, "failed to create server database\n");
        return EXIT_FAILURE;
    }
    
    // allocate client list based on configured max_clients
    client_list = (client_t **)calloc(config.max_clients, sizeof(client_t *));
    if (!client_list) {
        fprintf(stderr, "failed to allocate client list\n");
        dict_free(server_db);
        return EXIT_FAILURE;
    }
    
    // initialize persistence state
    server_persistence.changes_since_save = 0;
    server_persistence.last_save = time(NULL);
    
    // initialize replication
    replication_init();
    
    // load data from rdb file if it exists
    if (!load_rdb_from_file(server_db, "dump.rdb")) {
        fprintf(stderr, "warning: could not load rdb file, starting with empty db\n");
    }
    
    // initialize pub/sub
    pubsub_init();
    
    // start background threads
    pthread_t cleanup_thread, autosave_thread, repl_thread;
    pthread_create(&cleanup_thread, NULL, cleanup_expired_keys, NULL);
    pthread_create(&autosave_thread, NULL, auto_save_thread, NULL);
    pthread_create(&repl_thread, NULL, replication_thread, NULL);

    // select and run the server based on the configured concurrency model
    if (config.concurrency_model == CONCURRENCY_THREADED) {
        run_threaded_server(config.port);
    } else {
        run_eventloop_server(config.port);
    }

    // Wait for background threads to finish (optional, for clean shutdown)
    pthread_join(cleanup_thread, NULL);
    pthread_join(autosave_thread, NULL);
    pthread_join(repl_thread, NULL);
    
    // clean up
    dict_free(server_db);
    free(client_list);
    replication_cleanup();
    pubsub_cleanup();
    
    return 0;
}