#define _POSIX_C_SOURCE 200809L
#include "persistence.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <errno.h>

// helper function to get current time in ms
static uint64_t current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec) * 1000 + (tv.tv_usec / 1000);
}

// save a length-prefixed string
int rdb_save_string(FILE *fp, const char *str, size_t len) {
    if (fwrite(&len, sizeof(size_t), 1, fp) != 1) {
        return 0;
    }
    if (len > 0 && fwrite(str, len, 1, fp) != 1) {
        return 0;
    }
    return 1;
}

// load a length-prefixed string
int rdb_load_string(FILE *fp, char **str, size_t *len) {
    if (fread(len, sizeof(size_t), 1, fp) != 1) {
        return 0;
    }
    
    *str = malloc(*len + 1);
    if (!*str) return 0;
    
    if (*len > 0 && fread(*str, *len, 1, fp) != 1) {
        free(*str);
        return 0;
    }
    
    (*str)[*len] = '\0';
    return 1;
}

// save the entire database to a file
int save_rdb_to_file(dict *db, const char *filename) {
    FILE *fp;
    char temp_filename[256];
    int result = 0;
    
    // create a temporary file first
    snprintf(temp_filename, sizeof(temp_filename), "%s.tmp", filename);
    
    fp = fopen(temp_filename, "wb");
    if (!fp) {
        fprintf(stderr, "error: could not open %s for writing\n", temp_filename);
        return 0;
    }
    
    // write header
    if (fwrite(RDB_MAGIC, 4, 1, fp) != 1) goto cleanup;
    int version = RDB_VERSION;
    if (fwrite(&version, sizeof(int), 1, fp) != 1) goto cleanup;
    
    // write entries count
    if (fwrite(&db->used, sizeof(size_t), 1, fp) != 1) goto cleanup;
    
    // get current time
    uint64_t now = current_time_ms();
    
    // iterate through all entries
    for (size_t i = 0; i < db->size; i++) {
        dict_entry *entry = db->table[i];
        while (entry) {
            // skip expired keys
            if (entry->val->expire != 0 && entry->val->expire < now) {
                entry = entry->next;
                continue;
            }
            
            // save key
            size_t key_len = strlen(entry->key);
            if (!rdb_save_string(fp, entry->key, key_len)) goto cleanup;
            
            // save value type
            if (fwrite(&entry->val->type, sizeof(cc_type), 1, fp) != 1) goto cleanup;
            
            // save expiry (if any)
            uint8_t has_expiry = entry->val->expire != 0;
            if (fwrite(&has_expiry, sizeof(uint8_t), 1, fp) != 1) goto cleanup;
            
            if (has_expiry) {
                if (fwrite(&entry->val->expire, sizeof(uint64_t), 1, fp) != 1) goto cleanup;
            }
            
            // save value based on type
            switch (entry->val->type) {
                case CC_STRING: {
                    uint8_t cmd = RDB_SET;
                    if (fwrite(&cmd, sizeof(uint8_t), 1, fp) != 1) goto cleanup;
                    
                    // save string value
                    size_t str_len = strlen((char*)entry->val->ptr);
                    if (!rdb_save_string(fp, (char*)entry->val->ptr, str_len)) goto cleanup;
                    break;
                }
                // add other data types here as we implement them
                default:
                    break;
            }
            
            entry = entry->next;
        }
    }
    
    // write end marker
    uint8_t end_marker = RDB_END;
    if (fwrite(&end_marker, sizeof(uint8_t), 1, fp) != 1) goto cleanup;
    
    result = 1;
    
cleanup:
    fclose(fp);
    
    if (result) {
        // rename temp file to actual file only if save was successful
        if (rename(temp_filename, filename) != 0) {
            fprintf(stderr, "error: could not rename %s to %s\n", temp_filename, filename);
            return 0;
        }
    } else {
        // remove temp file if save failed
        unlink(temp_filename);
    }
    
    return result;
}

// load database from file
int load_rdb_from_file(dict *db, const char *filename) {
    FILE *fp;
    char magic[5] = {0};
    int version;
    int result = 0;
    
    fp = fopen(filename, "rb");
    if (!fp) {
        // it's not an error if the file doesn't exist yet
        if (errno == ENOENT) {
            return 1;
        }
        fprintf(stderr, "error: could not open %s for reading\n", filename);
        return 0;
    }
    
    // read and verify header
    if (fread(magic, 4, 1, fp) != 1 || memcmp(magic, RDB_MAGIC, 4) != 0) {
        fprintf(stderr, "error: invalid rdb file format\n");
        goto cleanup;
    }
    
    if (fread(&version, sizeof(int), 1, fp) != 1 || version != RDB_VERSION) {
        fprintf(stderr, "error: unsupported rdb version: %d\n", version);
        goto cleanup;
    }
    
    // read entries count
    size_t entries_count;
    if (fread(&entries_count, sizeof(size_t), 1, fp) != 1) {
        fprintf(stderr, "error: could not read entries count\n");
        goto cleanup;
    }
    
    // read all entries
    for (size_t i = 0; i < entries_count; i++) {
        // read key
        char *key;
        size_t key_len;
        if (!rdb_load_string(fp, &key, &key_len)) goto cleanup;
        
        // read value type
        cc_type type;
        if (fread(&type, sizeof(cc_type), 1, fp) != 1) {
            free(key);
            goto cleanup;
        }
        
        // read expiry (if any)
        uint8_t has_expiry;
        if (fread(&has_expiry, sizeof(uint8_t), 1, fp) != 1) {
            free(key);
            goto cleanup;
        }
        
        uint64_t expire = 0;
        if (has_expiry) {
            if (fread(&expire, sizeof(uint64_t), 1, fp) != 1) {
                free(key);
                goto cleanup;
            }
            
            // skip expired keys
            if (expire < current_time_ms()) {
                free(key);
                
                // skip the value data
                uint8_t cmd;
                if (fread(&cmd, sizeof(uint8_t), 1, fp) != 1) goto cleanup;
                
                if (cmd == RDB_SET) {
                    char *val;
                    size_t val_len;
                    if (!rdb_load_string(fp, &val, &val_len)) goto cleanup;
                    free(val);
                }
                
                continue;
            }
        }
        
        // read value command
        uint8_t cmd;
        if (fread(&cmd, sizeof(uint8_t), 1, fp) != 1) {
            free(key);
            goto cleanup;
        }
        
        switch (cmd) {
            case RDB_SET: {
                // read string value
                char *val;
                size_t val_len;
                if (!rdb_load_string(fp, &val, &val_len)) {
                    free(key);
                    goto cleanup;
                }
                
                // create string object
                cc_obj *obj = malloc(sizeof(cc_obj));
                if (!obj) {
                    free(key);
                    free(val);
                    goto cleanup;
                }
                
                obj->ptr = val;
                obj->type = CC_STRING;
                obj->expire = expire;
                obj->size = val_len + 1;
                obj->last_access = current_time_ms();
                
                // add to dictionary
                if (!dict_add(db, key, obj)) {
                    free(key);
                    free(val);
                    free(obj);
                    goto cleanup;
                }
                
                free(key); // dict_add makes a copy
                break;
            }
            // add other data types here as we implement them
            default:
                free(key);
                fprintf(stderr, "error: unknown command in rdb file: %d\n", cmd);
                goto cleanup;
        }
    }
    
    // check for end marker
    uint8_t end_marker;
    if (fread(&end_marker, sizeof(uint8_t), 1, fp) != 1 || end_marker != RDB_END) {
        fprintf(stderr, "error: missing end marker in rdb file\n");
        goto cleanup;
    }
    
    result = 1;
    
cleanup:
    fclose(fp);
    return result;
}

// fork a child process to save the database in the background
int background_save(dict *db, const char *filename) {
    pid_t child_pid = fork();
    
    if (child_pid == -1) {
        // fork failed
        fprintf(stderr, "error: fork failed: %s\n", strerror(errno));
        return 0;
    } else if (child_pid == 0) {
        // child process
        if (!save_rdb_to_file(db, filename)) {
            fprintf(stderr, "background save failed\n");
            exit(1);
        }
        exit(0);
    } else {
        // parent process
        printf("background saving started with pid %d\n", child_pid);
        return 1;
    }
}