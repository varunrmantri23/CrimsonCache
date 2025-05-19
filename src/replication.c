#define _POSIX_C_SOURCE 200809L
#include "replication.h"
#include "persistence.h"
#include "commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include "crimsoncache.h"

extern dict *server_db;
extern int server_port; 

// get current time in milliseconds
static uint64_t current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec) * 1000 + (tv.tv_usec / 1000);
}

replication_info_t server_repl;

// initialize replication
void replication_init(void) {
    server_repl.role = ROLE_PRIMARY;
    server_repl.state = REPL_STATE_NONE;
    server_repl.primary_fd = -1;
    server_repl.replicas = NULL;
    
    // generate a random replication id (40 chars)
    srand(time(NULL));
    for (int i = 0; i < 40; i++) {
        char c = rand() % 36;
        server_repl.replid[i] = (c < 10) ? c + '0' : c - 10 + 'a';
    }
    server_repl.replid[40] = '\0';
    
    server_repl.repl_offset = 0;
    pthread_mutex_init(&server_repl.replicas_mutex, NULL);
}

void sync_replica(int fd) {
    printf("performing initial sync with replica...\n");
    
    // Stats tracking
    int total_keys = 0;
    int synced_keys = 0;
    
    // Buffer for command formatting
    char cmd_buffer[4096];
    
    // Debug output the current database contents
    printf("primary database contains %zu active entries\n", server_db->used);
    
    // Iterate through all hash table buckets
    for (size_t i = 0; i < server_db->size; i++) {
        dict_entry *entry = server_db->table[i];
        
        // Process all entries in this bucket
        while (entry) {
            // Skip expired keys
            if (entry->val->expire != 0 && entry->val->expire < current_time_ms()) {
                entry = entry->next;
                continue;
            }
            
            total_keys++;
            
            // Only handle string values for now
            if (entry->val->type == CC_STRING) {
                // Format a SET command with proper quoting for string values
                int cmd_len;
                
                // Check if the value needs quoting (contains spaces or special chars)
                if (strchr((char*)entry->val->ptr, ' ') != NULL || 
                    strchr((char*)entry->val->ptr, '\t') != NULL ||
                    strchr((char*)entry->val->ptr, '"') != NULL) {
                    cmd_len = snprintf(cmd_buffer, sizeof(cmd_buffer), 
                                     "SET %s \"%s\"\r\n", 
                                     entry->key, 
                                     (char*)entry->val->ptr);
                } else {
                    cmd_len = snprintf(cmd_buffer, sizeof(cmd_buffer), 
                                     "SET %s %s\r\n", 
                                     entry->key, 
                                     (char*)entry->val->ptr);
                }
                
                // Log the exact command we're sending
                printf("Sending to replica: %s", cmd_buffer);
                
                // Send to the replica
                ssize_t written = write(fd, cmd_buffer, cmd_len);
                if (written == cmd_len) {
                    synced_keys++;
                    
                    // Small delay to allow command to be processed
                    struct timespec ts = {0, 10000000}; // 10ms
                    nanosleep(&ts, NULL);
                    
                    // If the key has an expiry, send EXPIRE command too
                    if (entry->val->expire != 0) {
                        uint64_t now = current_time_ms();
                        if (entry->val->expire > now) {
                            long ttl_sec = (entry->val->expire - now) / 1000;
                            if (ttl_sec > 0) {
                                cmd_len = snprintf(cmd_buffer, sizeof(cmd_buffer), 
                                                 "EXPIRE %s %ld\r\n", 
                                                 entry->key, ttl_sec);
                                write(fd, cmd_buffer, cmd_len);
                                
                                // Small delay for this command too
                                nanosleep(&ts, NULL);
                            }
                        }
                    }
                } else {
                    fprintf(stderr, "error syncing key %s: wrote %zd of %d bytes\n", 
                            entry->key, written, cmd_len);
                }
            }
            
            entry = entry->next;
        }
    }
    
    printf("initial sync completed: %d of %d keys synced\n", synced_keys, total_keys);
}

// add a new replica to the linked list
void add_replica(int fd, const char *ip, int port) {
    replica_t *replica = malloc(sizeof(replica_t));
    if (!replica) return;
    
    replica->fd = fd;
    replica->ip = strdup(ip);
    replica->port = port;
    replica->last_ack_time = time(NULL);
    
    pthread_mutex_lock(&server_repl.replicas_mutex);
    replica->next = server_repl.replicas;
    server_repl.replicas = replica;
    pthread_mutex_unlock(&server_repl.replicas_mutex);
    
    printf("new replica connected: %s:%d\n", ip, port);
    
    // perform initial synchronization
    sync_replica(fd);
}

// // Handle REPLCONF command
// cmd_result replconf_command(int client_sock, int argc, char **argv, dict *db) {
//     (void)db; // Unused
    
//     // Handle REPLCONF listening-port <port>
//     if (argc >= 3 && strcasecmp(argv[1], "listening-port") == 0) {
//         int port = atoi(argv[2]);
        
//         // Get client IP address
//         struct sockaddr_in addr;
//         socklen_t addr_len = sizeof(addr);
//         getpeername(client_sock, (struct sockaddr*)&addr, &addr_len);
//         char ip[INET_ADDRSTRLEN];
//         inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
        
//         printf("Received REPLCONF from %s:%d\n", ip, port);
        
//         // Add replica to the list (this will also trigger sync_replica)
//         add_replica(client_sock, ip, port);
        
//         reply_string(client_sock, "OK");
//         return CMD_OK;
//     }
    
//     // Handle other REPLCONF commands
//     reply_string(client_sock, "OK");
//     return CMD_OK;
// }

// remove a replica from the linked list
void remove_replica(int fd) {
    pthread_mutex_lock(&server_repl.replicas_mutex);
    
    replica_t *curr = server_repl.replicas;
    replica_t *prev = NULL;
    
    while (curr) {
        if (curr->fd == fd) {
            if (prev) {
                prev->next = curr->next;
            } else {
                server_repl.replicas = curr->next;
            }
            
            printf("replica disconnected: %s:%d\n", curr->ip, curr->port);
            free(curr->ip);
            free(curr);
            break;
        }
        
        prev = curr;
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&server_repl.replicas_mutex);
}

// set this server as a replica of the specified primary
int replication_set_primary(const char *host, int port) {
    // clean up any existing replica connection
    if (server_repl.primary_fd != -1) {
        close(server_repl.primary_fd);
        server_repl.primary_fd = -1;
    }
    
    // update server role and primary info
    server_repl.role = ROLE_REPLICA;
    strncpy(server_repl.primary_host, host, sizeof(server_repl.primary_host) - 1);
    server_repl.primary_port = port;
    server_repl.state = REPL_STATE_CONNECTING;
    
    // resolve primary hostname
    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    int rv = getaddrinfo(host, port_str, &hints, &servinfo);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 0;
    }
    
    // connect to primary
    int fd = -1;
    struct addrinfo *p;
    for (p = servinfo; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) continue;
        
        if (connect(fd, p->ai_addr, p->ai_addrlen) == -1) {
            close(fd);
            continue;
        }
        
        break;
    }
    
    freeaddrinfo(servinfo);
    
    if (p == NULL) {
        fprintf(stderr, "failed to connect to primary %s:%d\n", host, port);
        return 0;
    }
    
    // set socket to non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    server_repl.primary_fd = fd;
    server_repl.state = REPL_STATE_SYNC;
    
    // send replication command to primary
    char replconf_cmd[64];
    snprintf(replconf_cmd, sizeof(replconf_cmd), "REPLCONF listening-port %d\r\n", 
             server_port);
    write(fd, replconf_cmd, strlen(replconf_cmd));
    
    // send PSYNC command to request full resync
    char psync_cmd[256];
    snprintf(psync_cmd, sizeof(psync_cmd), "PSYNC ? -1\r\n");
    write(fd, psync_cmd, strlen(psync_cmd));
    
    printf("connected to primary %s:%d\n", host, port);
    return 1;
}

// disconnect from primary
void replication_unset_primary(void) {
    if (server_repl.primary_fd != -1) {
        close(server_repl.primary_fd);
        server_repl.primary_fd = -1;
    }
    
    server_repl.role = ROLE_PRIMARY;
    server_repl.state = REPL_STATE_NONE;
    printf("disconnected from primary, now acting as primary\n");
}

// propagate commands to replicas
void replication_feed_slaves(char *cmd, size_t cmd_len) {
    if (server_repl.role != ROLE_PRIMARY) return;
    
    char *propagate_cmd = cmd;
    size_t propagate_len = cmd_len;
    char *temp_cmd = NULL;
    
    if (cmd_len < 2 || cmd[cmd_len-2] != '\r' || cmd[cmd_len-1] != '\n') {
        temp_cmd = malloc(cmd_len + 3);
        if (temp_cmd) {
            memcpy(temp_cmd, cmd, cmd_len);
            temp_cmd[cmd_len] = '\r';
            temp_cmd[cmd_len + 1] = '\n';
            temp_cmd[cmd_len + 2] = '\0';
            propagate_cmd = temp_cmd;
            propagate_len = cmd_len + 2;
        }
    }
    
    int to_remove[100] = {0};
    int remove_count = 0;
    
    pthread_mutex_lock(&server_repl.replicas_mutex);
    
    replica_t *curr = server_repl.replicas;
    replica_t *next;
    
    while (curr) {
        next = curr->next; 
        
        ssize_t nwritten = write(curr->fd, propagate_cmd, propagate_len);
        if (nwritten != (ssize_t)propagate_len) {
            if (nwritten == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                fprintf(stderr, "would block writing to replica (will retry)\n");
            } else {
                fprintf(stderr, "error writing to replica, marking it for removal\n");
                if (remove_count < 100) {
                    to_remove[remove_count++] = curr->fd;
                }
            }
        } else {
            // update replication offset
            server_repl.repl_offset += propagate_len;
            curr->last_ack_time = time(NULL);
        }
        
        curr = next;
    }
    
    pthread_mutex_unlock(&server_repl.replicas_mutex);
    
    // Now remove any marked replicas after releasing the mutex
    for (int i = 0; i < remove_count; i++) {
        remove_replica(to_remove[i]);
    }
    
    if (temp_cmd) {
        free(temp_cmd);
    }
}

// get replication info for the INFO command
void replication_info_append(char *info, size_t *len) {
    char buf[1024];
    // int n;
    
    snprintf(buf, sizeof(buf),
                "# Replication\r\n"
                "role:%s\r\n"
                "connected_replicas:%d\r\n"
                "repl_offset:%lu\r\n"
                "repl_id:%s\r\n",
                replication_get_role_name(),
                count_replicas(),
                server_repl.repl_offset,
                server_repl.replid);
    
    strncat(info, buf, *len - strlen(info) - 1);
    
    if (server_repl.role == ROLE_REPLICA) {
        snprintf(buf, sizeof(buf),
                    "primary_host:%s\r\n"
                    "primary_port:%d\r\n"
                    "primary_link_status:%s\r\n",
                    server_repl.primary_host,
                    server_repl.primary_port,
                    server_repl.state == REPL_STATE_CONNECTED ? "up" : "down");
        
        strncat(info, buf, *len - strlen(info) - 1);
    }
}

// get server role as string
const char* replication_get_role_name(void) {
    return server_repl.role == ROLE_PRIMARY ? "master" : "slave";
}

// count number of connected replicas
int count_replicas(void) {
    int count = 0;
    pthread_mutex_lock(&server_repl.replicas_mutex);
    
    replica_t *curr = server_repl.replicas;
    while (curr) {
        count++;
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&server_repl.replicas_mutex);
    return count;
}

// clean up replication resources
void replication_cleanup(void) {
    // close primary connection if we're a replica
    if (server_repl.primary_fd != -1) {
        close(server_repl.primary_fd);
    }
    
    // free all replica structures
    pthread_mutex_lock(&server_repl.replicas_mutex);
    
    replica_t *curr = server_repl.replicas;
    while (curr) {
        replica_t *next = curr->next;
        close(curr->fd);
        free(curr->ip);
        free(curr);
        curr = next;
    }
    server_repl.replicas = NULL;
    
    pthread_mutex_unlock(&server_repl.replicas_mutex);
    pthread_mutex_destroy(&server_repl.replicas_mutex);
}