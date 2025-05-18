#define _POSIX_C_SOURCE 200809L
#include "commands.h"
#include "persistence.h"
#include "replication.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "crimsoncache.h"

extern void track_command_change(void);
extern volatile sig_atomic_t server_running;

// fallback implementations if not available
#ifndef HAVE_STRDUP
static char* my_strdup(const char* s) {
    size_t len = strlen(s) + 1;
    char* new_str = malloc(len);
    if (new_str) {
        memcpy(new_str, s, len);
    }
    return new_str;
}
#define strdup my_strdup
#endif

#ifndef HAVE_STRCASECMP
static int my_strcasecmp(const char* s1, const char* s2) {
    while (*s1 && (tolower((unsigned char)*s1) == tolower((unsigned char)*s2))) {
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}
#define strcasecmp my_strcasecmp
#endif

// command table
static command_def commands[] = {
    {"ping", ping_command, 1, 2},
    {"set", set_command, 3, -1},
    {"get", get_command, 2, 2},
    {"del", del_command, 2, -1},
    {"exists", exists_command, 2, -1},
    {"expire", expire_command, 3, 3},
    {"ttl", ttl_command, 2, 2},
    {"save", save_command, 1, 1},
    {"bgsave", bgsave_command, 1, 1},
    {"replicaof", replicaof_command, 3, 3},
    {"role", role_command, 1, 1},
    {"incr", incr_command, 2, 2},
    {"replconf", replconf_command, 2, -1},
    {NULL, NULL, 0, 0}  // sentinel to mark end of array
};

// get current time in milliseconds
static uint64_t current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec) * 1000 + (tv.tv_usec / 1000);
}

// tokenize the input command
char** tokenize_command(char *input, int *argc) {
    char **tokens = NULL;
    int max_tokens = 10;  // start with space for 10 tokens
    int count = 0;
    
    // remove trailing \r\n if present
    size_t len = strlen(input);
    if (len >= 2 && input[len-2] == '\r' && input[len-1] == '\n') {
        input[len-2] = '\0';
    } else if (len >= 1 && input[len-1] == '\n') {
        input[len-1] = '\0';
    }
    
    // allocate initial token array
    tokens = malloc(sizeof(char*) * max_tokens);
    if (!tokens) {
        *argc = 0;
        return NULL;
    }
    
    char *p = input;
    int state = 0;  // 0: skipping spaces, 1: in token, 2: in quoted token
    char *start = NULL;
    
    while (*p) {
        char c = *p;
        
        switch (state) {
            case 0:  
                if (isspace((unsigned char)c)) {
                    p++;
                } else if (c == '"') {
                    start = p + 1;
                    state = 2;
                    p++;
                } else {
                    start = p;
                    state = 1;
                    p++;
                }
                break;
                
            case 1:  
                if (isspace((unsigned char)c)) {
                    // end of token
                    *p = '\0';
                    
                    // Resize array if needed
                    if (count >= max_tokens) {
                        max_tokens *= 2;
                        char **new_tokens = realloc(tokens, sizeof(char*) * max_tokens);
                        if (!new_tokens) goto cleanup;
                        tokens = new_tokens;
                    }
                    
                    
                    tokens[count] = strdup(start);
                    if (!tokens[count]) goto cleanup;
                    count++;
                    
                    state = 0;
                    p++;
                } else {
                    p++;
                }
                break;
                
            case 2:  
                if (c == '"' && (p == input || *(p-1) != '\\')) {
                    // end of quoted token
                    *p = '\0';
                    
                    // resize array if needed
                    if (count >= max_tokens) {
                        max_tokens *= 2;
                        char **new_tokens = realloc(tokens, sizeof(char*) * max_tokens);
                        if (!new_tokens) goto cleanup;
                        tokens = new_tokens;
                    }

                    tokens[count] = strdup(start);
                    if (!tokens[count]) goto cleanup;
                    count++;
                    
                    state = 0;
                    p++;
                } else {
                    p++;
                }
                break;
        }
    }
    
    if (state == 1) {
        // resize array if needed
        if (count >= max_tokens) {
            max_tokens++;
            char **new_tokens = realloc(tokens, sizeof(char*) * max_tokens);
            if (!new_tokens) goto cleanup;
            tokens = new_tokens;
        }
        
        tokens[count] = strdup(start);
        if (!tokens[count]) goto cleanup;
        count++;
    }
    
    *argc = count;
    return tokens;
    
cleanup:
    for (int i = 0; i < count; i++) {
        free(tokens[i]);
    }
    free(tokens);
    *argc = 0;
    return NULL;
}

void free_tokens(char **tokens, int count) {
    if (!tokens) return;
    
    for (int i = 0; i < count; i++) {
        free(tokens[i]);
    }
    free(tokens);
}

extern void track_command_change();

cmd_result execute_command(int client_sock, char *input, dict *db) {
    int argc = 0;
    char **argv = tokenize_command(input, &argc);
    cmd_result result = CMD_UNKNOWN;
    
    if (!argv || argc == 0) {
        if (argv) free_tokens(argv, argc);
        reply_error(client_sock, "ERR empty command");
        return CMD_ERR;
    }
    
    for (size_t i = 0; i < strlen(argv[0]); i++) {
        argv[0][i] = tolower(argv[0][i]);
    }
    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            // check argument count
            if ((commands[i].min_args > 0 && argc < commands[i].min_args) ||
                (commands[i].max_args > 0 && argc > commands[i].max_args)) {
                reply_error(client_sock, "ERR wrong number of arguments");
                result = CMD_ERR;
            } else {
                result = commands[i].handler(client_sock, argc, argv, db);
            }
            break;
        }
    }
    
    if (result == CMD_UNKNOWN) {
        reply_error(client_sock, "ERR unknown command");
        result = CMD_ERR;
    }

    // track changes for write commands and propagate to replicas if needed
    if (result == CMD_OK && (
        strcasecmp(argv[0], "set") == 0 ||
        strcasecmp(argv[0], "del") == 0 ||
        strcasecmp(argv[0], "expire") == 0 ||
        strcasecmp(argv[0], "incr") == 0)) {
        
        track_command_change();
        
        // propagate to replicas if we're a primary and this is a client command
        if (server_repl.role == ROLE_PRIMARY && client_sock >= 0) {
            // ensure the command ends with \r\n for proper processing
            char *propagate_cmd;
            if (input[strlen(input)-1] != '\n') {
                propagate_cmd = malloc(strlen(input) + 3);
                if (propagate_cmd) {
                    sprintf(propagate_cmd, "%s\r\n", input);
                    replication_feed_slaves(propagate_cmd, strlen(propagate_cmd));
                    free(propagate_cmd);
                }
            } else {
                replication_feed_slaves(input, strlen(input));
            }
        }
    }
    
    free_tokens(argv, argc);
    return result;
}

// response formatters
void reply_string(int client_sock, const char *str) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "+%s\r\n", str);
    write(client_sock, buffer, strlen(buffer));
}

void reply_error(int client_sock, const char *err) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "-%s\r\n", err);
    write(client_sock, buffer, strlen(buffer));
}

void reply_integer(int client_sock, long long num) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), ":%lld\r\n", num);
    write(client_sock, buffer, strlen(buffer));
}

void reply_bulk(int client_sock, const char *str) {
    char buffer[1024];
    size_t len = str ? strlen(str) : 0;
    
    // Format: $<length>\r\n<data>\r\n
    snprintf(buffer, sizeof(buffer), "$%zu\r\n", len);
    write(client_sock, buffer, strlen(buffer));
    
    if (str && len > 0) {
        write(client_sock, str, len);
        write(client_sock, "\r\n", 2);
    }
}

void reply_null_bulk(int client_sock) {
    write(client_sock, "$-1\r\n", 5);
}

// command implementations
cmd_result ping_command(int client_sock, int argc, char **argv, dict *db) {
    (void)db;
    
    if (argc > 1) {
        reply_bulk(client_sock, argv[1]);
    } else {
        reply_string(client_sock, "PONG");
    }
    return CMD_OK;
}

cmd_result set_command(int client_sock, int argc, char **argv, dict *db) {
    const char *key = argv[1];
    const char *value = argv[2];
    uint64_t expire_ms = 0;
    
    // Check for EX/PX option
    if (argc >= 5) {
        if (strcasecmp(argv[3], "EX") == 0) {
            // EX = seconds
            expire_ms = current_time_ms() + (atoll(argv[4]) * 1000);
        } else if (strcasecmp(argv[3], "PX") == 0) {
            // PX = milliseconds
            expire_ms = current_time_ms() + atoll(argv[4]);
        }
    }
    
    // create string object
    cc_obj *obj = malloc(sizeof(cc_obj));
    if (!obj) {
        reply_error(client_sock, "ERR out of memory");
        return CMD_ERR;
    }
    
    obj->ptr = strdup(value);
    if (!obj->ptr) {
        free(obj);
        reply_error(client_sock, "ERR out of memory");
        return CMD_ERR;
    }
    
    obj->type = CC_STRING;
    obj->expire = expire_ms;
    obj->size = strlen(value) + 1;
    obj->last_access = current_time_ms();
    
    if (dict_add(db, key, obj)) {
        reply_string(client_sock, "OK");
        return CMD_OK;
    } else {
        free(obj->ptr);
        free(obj);
        reply_error(client_sock, "ERR could not set key");
        return CMD_ERR;
    }
}

cmd_result get_command(int client_sock, int argc, char **argv, dict *db) {
    (void)argc; // unused parameter
    
    cc_obj *obj = dict_get(db, argv[1]);
    if (obj && obj->type == CC_STRING) {
        reply_bulk(client_sock, (char*)obj->ptr);
    } else {
        reply_null_bulk(client_sock);
    }
    return CMD_OK;
}

cmd_result del_command(int client_sock, int argc, char **argv, dict *db) {
    int deleted = 0;
    
    for (int i = 1; i < argc; i++) {
        if (dict_delete(db, argv[i])) {
            deleted++;
        }
    }
    
    reply_integer(client_sock, deleted);
    return CMD_OK;
}

cmd_result exists_command(int client_sock, int argc, char **argv, dict *db) {
    int count = 0;
    
    for (int i = 1; i < argc; i++) {
        if (dict_get(db, argv[i]) != NULL) {
            count++;
        }
    }
    
    reply_integer(client_sock, count);
    return CMD_OK;
}

cmd_result expire_command(int client_sock, int argc, char **argv, dict *db) {
    (void)argc; // Unused parameter
    
    cc_obj *obj = dict_get(db, argv[1]);
    if (!obj) {
        reply_integer(client_sock, 0);
        return CMD_OK;
    }
    
    long seconds = atol(argv[2]);
    obj->expire = current_time_ms() + (seconds * 1000);
    
    reply_integer(client_sock, 1);
    return CMD_OK;
}

cmd_result ttl_command(int client_sock, int argc, char **argv, dict *db) {
    (void)argc; // Unused parameter
    
    cc_obj *obj = dict_get(db, argv[1]);
    if (!obj) {
        reply_integer(client_sock, -2);
        return CMD_OK;
    }
    
    if (obj->expire == 0) {
        reply_integer(client_sock, -1);
        return CMD_OK;
    }
    
    uint64_t now = current_time_ms();
    if (obj->expire <= now) {
        // key has expired but hasn't been removed yet
        reply_integer(client_sock, -2);
        return CMD_OK;
    }
    
    long long ttl = (obj->expire - now) / 1000;
    reply_integer(client_sock, ttl);
    return CMD_OK;
}

cmd_result save_command(int client_sock, int argc, char **argv, dict *db) {
    (void)argc; // unused
    (void)argv; // unused
    
    printf("Manual SAVE command received, saving database to disk...\n");
    
    if (save_rdb_to_file(db, "dump.rdb")) {
        printf("Manual save completed successfully\n");
        reply_string(client_sock, "OK");
        return CMD_OK;
    } else {
        fprintf(stderr, "Manual save failed\n");
        reply_error(client_sock, "ERR failed to save db");
        return CMD_ERR;
    }
}

cmd_result bgsave_command(int client_sock, int argc, char **argv, dict *db) {
    (void)argc; // unused
    (void)argv; // unused
    
    printf("Manual BGSAVE command received, starting background save...\n");
    
    if (background_save(db, "dump.rdb")) {
        reply_string(client_sock, "Background saving started");
        return CMD_OK;
    } else {
        fprintf(stderr, "Failed to start background save\n");
        reply_error(client_sock, "ERR failed to start background save");
        return CMD_ERR;
    }
}

// replicaof command - configure server as replica of another or as primary
cmd_result replicaof_command(int client_sock, int argc, char **argv, dict *db) {
    (void)argc; // unused
    (void)db;   // unused

    // replicaof no one = become primary
    if (strcasecmp(argv[1], "NO") == 0 && strcasecmp(argv[2], "ONE") == 0) {
        replication_unset_primary();
        reply_string(client_sock, "OK");
        return CMD_OK;
    }

    // replicaof host port = become replica
    const char *host = argv[1];
    int port = atoi(argv[2]);
    
    if (port <= 0 || port > 65535) {
        reply_error(client_sock, "ERR invalid port");
        return CMD_ERR;
    }
    
    if (replication_set_primary(host, port)) {
        reply_string(client_sock, "OK");
        return CMD_OK;
    } else {
        reply_error(client_sock, "ERR couldn't connect to primary");
        return CMD_ERR;
    }
}

// role command - return role of server (primary or replica)
cmd_result role_command(int client_sock, int argc, char **argv, dict *db) {
    (void)argc; // unused
    (void)argv; // unused
    (void)db;   // unused
    
    char response[1024];
    
    if (server_repl.role == ROLE_PRIMARY) {
        // format: *3\r\n$6\r\nmaster\r\n:<repl_offset>\r\n*<num_replicas>\r\n...
        int written = snprintf(response, sizeof(response), 
                              "*3\r\n$6\r\nmaster\r\n:%lu\r\n*%d\r\n", 
                              server_repl.repl_offset, 
                              count_replicas());
        
        // add replica info
        pthread_mutex_lock(&server_repl.replicas_mutex);
        replica_t *curr = server_repl.replicas;
        while (curr && written < (int)sizeof(response) - 100) {
            written += snprintf(response + written, sizeof(response) - written,
                              "*3\r\n$%zu\r\n%s\r\n:%d\r\n:%ld\r\n",
                              strlen(curr->ip), curr->ip, curr->port,
                              time(NULL) - curr->last_ack_time);
            curr = curr->next;
        }
        pthread_mutex_unlock(&server_repl.replicas_mutex);
        
    } else {
        // format: *5\r\n$5\r\nslave\r\n$<host_len>\r\n<host>\r\n:<port>\r\n$<state_len>\r\n<state>\r\n:<offset>\r\n
        snprintf(response, sizeof(response),
                "*5\r\n$5\r\nslave\r\n$%zu\r\n%s\r\n:%d\r\n$%zu\r\n%s\r\n:%lu\r\n",
                strlen(server_repl.primary_host), server_repl.primary_host,
                server_repl.primary_port,
                strlen(server_repl.state == REPL_STATE_CONNECTED ? "connected" : "connecting"),
                server_repl.state == REPL_STATE_CONNECTED ? "connected" : "connecting",
                server_repl.repl_offset);
    }
    
    write(client_sock, response, strlen(response));
    return CMD_OK;
}

// INCR command implementation
cmd_result incr_command(int client_sock, int argc, char **argv, dict *db) {
    (void)argc; // unused parameter
    
    const char *key = argv[1];
    
    // Get current value
    cc_obj *obj = dict_get(db, key);
    long long value = 0;
    
    if (obj) {
        // Key exists, check if it's a string we can convert to number
        if (obj->type != CC_STRING) {
            reply_error(client_sock, "ERR value is not an integer or out of range");
            return CMD_ERR;
        }
        
        // Try to parse as integer
        char *endptr;
        value = strtoll((char*)obj->ptr, &endptr, 10);
        
        if (*endptr != '\0') {
            reply_error(client_sock, "ERR value is not an integer or out of range");
            return CMD_ERR;
        }
    }
    
    // Increment the value
    value++;
    
    // Convert back to string
    char new_val[32];
    snprintf(new_val, sizeof(new_val), "%lld", value);
    
    // Create new object
    cc_obj *new_obj = malloc(sizeof(cc_obj));
    if (!new_obj) {
        reply_error(client_sock, "ERR out of memory");
        return CMD_ERR;
    }
    
    new_obj->ptr = strdup(new_val);
    if (!new_obj->ptr) {
        free(new_obj);
        reply_error(client_sock, "ERR out of memory");
        return CMD_ERR;
    }
    
    new_obj->type = CC_STRING;
    new_obj->expire = obj ? obj->expire : 0; // Preserve expiry if exists
    new_obj->size = strlen(new_val) + 1;
    new_obj->last_access = current_time_ms();
    
    if (dict_add(db, key, new_obj)) {
        reply_integer(client_sock, value);
        return CMD_OK;
    } else {
        free(new_obj->ptr);
        free(new_obj);
        reply_error(client_sock, "ERR could not set key");
        return CMD_ERR;
    }
}

// implement the REPLCONF command handler
cmd_result replconf_command(int client_sock, int argc, char **argv, dict *db) {
    (void)db; // unused
    
    // handle REPLCONF listening-port <port>
    if (argc >= 3 && strcasecmp(argv[1], "listening-port") == 0) {
        int port = atoi(argv[2]);
        
        printf("Received REPLCONF listening-port %d from replica\n", port);
        
        // get client IP address
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        getpeername(client_sock, (struct sockaddr*)&addr, &addr_len);
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
        
        // add replica to the list (which will perform initial sync)
        add_replica(client_sock, ip, port);
        
        reply_string(client_sock, "OK");
        return CMD_OK;
    }
    
    // handle other REPLCONF commands
    reply_string(client_sock, "OK");
    return CMD_OK;
}




