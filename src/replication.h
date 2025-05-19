#ifndef REPLICATION_H
#define REPLICATION_H

#include "dict.h"
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

// replication roles
typedef enum {
    ROLE_PRIMARY,
    ROLE_REPLICA
} server_role_t;

// replica state
typedef enum {
    REPL_STATE_NONE,           // not a replica
    REPL_STATE_CONNECTING,     // connecting to primary
    REPL_STATE_SYNC,           // performing initial sync
    REPL_STATE_CONNECTED       // connected and receiving updates
} replica_state_t;

// replica info
typedef struct replica {
    int fd;                     // socket file descriptor
    char *ip;                   // ip address
    int port;                   // port
    time_t last_ack_time;       // last time replica acknowledged a command
    struct replica *next;       // next replica in linked list
} replica_t;

// replication info
typedef struct {
    server_role_t role;          // current server role
    char primary_host[256];      // primary hostname
    int primary_port;            // primary port
    replica_state_t state;       // state when in replica mode
    int primary_fd;              // socket connected to primary when in replica mode
    
    // primary-specific fields
    char replid[41];             // replication id
    uint64_t repl_offset;        // replication offset
    replica_t *replicas;         // linked list of connected replicas
    pthread_mutex_t replicas_mutex; // mutex for thread-safe access to replicas list
} replication_info_t;

// global replication info
extern replication_info_t server_repl;

// initialize replication system
void replication_init(void);

// set this server as a replica of the specified primary
int replication_set_primary(const char *host, int port);

// disconnect from primary
void replication_unset_primary(void);

// process commands when in replica mode
void replication_feed_slaves(char *cmd, size_t cmd_len);

// get replication info for the INFO command
void replication_info_append(char *info, size_t *len);

// get server role as string
const char* replication_get_role_name(void);

// count number of connected replicas
int count_replicas(void);

// clean up replication resources
void replication_cleanup(void);

// synchronize a new replica with current data
void sync_replica(int fd);

// add a new replica to the system
void add_replica(int fd, const char *ip, int port);

// remove a replica from the system
void remove_replica(int fd);

#endif /* REPLICATION_H */