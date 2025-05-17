#define _POSIX_C_SOURCE 200809L
#include "commands.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

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