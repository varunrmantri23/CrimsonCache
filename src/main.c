#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> //for system calls
#include <signal.h>
#include <netinet/in.h> //internet address structs
#include <sys/socket.h> // socket functions (socket, bind, listen, accept)
#include <arpa/inet.h> 
#include <pthread.h> // POSIX threads for concurrency 

#define DEFAULT_PORT 6379 //redis standard port 
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 100

volatile sig_atomic_t server_running = 1;

void handle_signal(int sig) { //signal handler - handles ctrl+c or kill 
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\nShutting down server...\n");
        server_running = 0;
    }
} 

typedef struct {
    int socket;
    struct sockaddr_in address;
} client_t;

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
        
        // simple command handling - just for PING/ping return PONG
        if (strncmp(buffer, "PING\r\n", 6) == 0 || 
            strncmp(buffer, "ping\r\n", 6) == 0) {
            send(client_sock, "+PONG\r\n", 7, 0);
        } else {
            send(client_sock, "-ERR unknown command\r\n", 22, 0);
        }
    }
    
    close(client_sock);
    free(client);
    return NULL;
}

int main(int argc, char *argv[]) {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int port = DEFAULT_PORT;
    pthread_t thread_id;
    
    // handle command line args for port
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    printf("CrimsonCache starting on port %d\n", port);
    
    // set up signal handling
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    // create server socket
    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }
    
    // set SO_REUSEADDR option
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }
    
    // set up server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
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
    return 0;
}