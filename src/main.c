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
#include "crimsoncache.h"
#include "commands.h"
#include "persistence.h"

// global dictionary (server database)
dict *server_db = NULL;

volatile sig_atomic_t server_running = 1;

// persistence-related global variables
struct {
    int changes_since_save;
    time_t last_save;
    int save_after_changes;
    int save_after_seconds;
} server_persistence = {
    .changes_since_save = 0,
    .last_save = 0,
    .save_after_changes = 1000,  // save after 1000 changes
    .save_after_seconds = 300    // save every 5 minutes
};

void handle_signal(int sig) { //signal handler - handles ctrl+c or kill 
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\nShutting down server...\n");
        server_running = 0;
    }
}

void *cleanup_expired_keys(void *arg) {
    (void)arg;  // explicitly mark parameter as unused
    
    while (server_running) {
        // clean expired keys every second
        dict_clear_expired(server_db);
        sleep(1);
    }
    return NULL;
}

// function to track changes when commands are executed
void track_command_change() {
    server_persistence.changes_since_save++;
}

// function to periodically check if we need to save
void *auto_save_thread(void *arg) {
    (void)arg;  // unused parameter
    
    while (server_running) {
        sleep(1);  // check every second
        
        time_t now = time(NULL);
        int time_since_save = now - server_persistence.last_save;
        
        if ((server_persistence.changes_since_save >= server_persistence.save_after_changes) ||
            (time_since_save >= server_persistence.save_after_seconds && server_persistence.changes_since_save > 0)) {
            
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

void *handle_client(void *arg) {
    client_t *client = (client_t *)arg;
    int client_sock = client->socket;
    char buffer[BUFFER_SIZE];
    char client_addr[INET_ADDRSTRLEN];
    
    inet_ntop(AF_INET, &client->address.sin_addr, client_addr, INET_ADDRSTRLEN);
    printf("New client connected: %s:%d\n", 
           client_addr, ntohs(client->address.sin_port));
    
    while (server_running) {
        int bytes_read = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) {
            printf("Client %s:%d disconnected\n", 
                   client_addr, ntohs(client->address.sin_port));
            break;
        }
        
        buffer[bytes_read] = '\0';
        
        // process command using the command module
        execute_command(client_sock, buffer, server_db);
        
        // track changes for persistence
        track_command_change();
    }
    
    close(client_sock);
    free(client);
    return NULL;
}

typedef enum {
    CONCURRENCY_THREADED,  // one thread per client (current)
    CONCURRENCY_EVENTLOOP  // redis-style event loop - future
} concurrency_model_t;

// function to parse concurrency model from command line arguments
concurrency_model_t parse_concurrency_model(int argc, char *argv[]) {
    if (argc > 2 && strcmp(argv[2], "eventloop") == 0) {
        return CONCURRENCY_EVENTLOOP;
    }
    return CONCURRENCY_THREADED;
}

// function to run threaded server
void run_threaded_server() {
    int server_sock, client_sock;
    struct sockaddr_in6 server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int port = DEFAULT_PORT;
    pthread_t thread_id;
    
    // handle command line args for port
    printf("CrimsonCache starting on port %d\n", port);
    
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
    if (listen(server_sock, MAX_CLIENTS) < 0) {
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
    
    // accept connections and handle clients
    while (server_running) {
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            if (server_running) perror("Accept failed");
            continue;
        }
        
        client_t *client = (client_t *)malloc(sizeof(client_t));
        if (client == NULL) {
            perror("Failed to allocate memory for client");
            close(client_sock);
            continue;
        }
        
        client->socket = client_sock;
        client->address = client_addr;
        
        if (pthread_create(&thread_id, NULL, handle_client, (void *)client) != 0) {
            perror("Failed to create thread");
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

// later 
void run_eventloop_server() {
    printf("Event loop server is not implemented yet.\n");
}

int main(int argc, char *argv[]) {
    // initialize server database
    server_db = dict_create(1024);
    server_persistence.last_save = time(NULL);
    if (!server_db) {
        fprintf(stderr, "Failed to create server database\n");
        return EXIT_FAILURE;
    }
    
    // load data from rdb file if it exists
    if (!load_rdb_from_file(server_db, "dump.rdb")) {
        fprintf(stderr, "Warning: could not load RDB file, starting with empty DB\n");
    }
    
    // parse command line args for model selection
    concurrency_model_t model = parse_concurrency_model(argc, argv);
    
    if (model == CONCURRENCY_THREADED) {
        run_threaded_server();
    } else {
        run_eventloop_server();
    }
    
    // clean up
    dict_free(server_db);
    
    return 0;
}