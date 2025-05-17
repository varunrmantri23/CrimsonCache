#ifndef COMMANDS_H
#define COMMANDS_H

#include "dict.h"
#include <stddef.h>

// RESP protocol types
#define RESP_STRING '+'
#define RESP_ERROR '-'
#define RESP_INTEGER ':'
#define RESP_BULK '$'
#define RESP_ARRAY '*'

// Command handling result codes
typedef enum {
    CMD_OK,
    CMD_ERR,
    CMD_UNKNOWN
} cmd_result;

// Command handler function type
typedef cmd_result (*cmd_handler)(int client_sock, int argc, char **argv, dict *db);

// Command definition
typedef struct command_def {
    char *name;
    cmd_handler handler;
    int min_args;  // Minimum number of arguments (including command name)
    int max_args;  // Maximum arguments (-1 for unlimited)
} command_def;

// Command parsing and execution
char** tokenize_command(char *input, int *argc);
void free_tokens(char **tokens, int count);
cmd_result execute_command(int client_sock, char *input, dict *db);

// Response formatting
void reply_string(int client_sock, const char *str);
void reply_error(int client_sock, const char *err);
void reply_integer(int client_sock, long long num);
void reply_bulk(int client_sock, const char *str);
void reply_null_bulk(int client_sock);

// Command implementations
cmd_result ping_command(int client_sock, int argc, char **argv, dict *db);
cmd_result set_command(int client_sock, int argc, char **argv, dict *db);
cmd_result get_command(int client_sock, int argc, char **argv, dict *db);
cmd_result del_command(int client_sock, int argc, char **argv, dict *db);
cmd_result exists_command(int client_sock, int argc, char **argv, dict *db);
cmd_result expire_command(int client_sock, int argc, char **argv, dict *db);
cmd_result ttl_command(int client_sock, int argc, char **argv, dict *db);

#endif /* COMMANDS_H */