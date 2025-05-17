#ifndef DICT_H
#define DICT_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    CC_STRING, 
    CC_LIST, 
    CC_SET,
    CC_INT,
    CC_FLOAT,
    CC_BOOL
} cc_type;

// value object structure
typedef struct cc_obj {
    void *ptr;            // pointer to actual data
    cc_type type;         // type of data
    uint64_t expire;      // expiration timestamp (0 = no expiry)
    size_t size;          // size of data in bytes
    uint64_t last_access; // time of last access -- for lru eviction
} cc_obj;

// Dictionary entry
typedef struct dict_entry {
    char *key;                 // key
    cc_obj *val;               // value
    struct dict_entry *next;   // next entry in the linked list (hash collision)
} dict_entry;

// main dictionary structure
typedef struct dict {
    dict_entry **table;    // hash table
    size_t size;           // size of the hash table
    size_t used;           // number of entries in the hash table
    size_t mask;           // bitmask for fast modulo (size-1)
    size_t max_memory;     // max limit for lru
    size_t used_memory;    // current memory usage
} dict;

// dictionary functions
dict* dict_create(size_t initial_size);
void dict_free(dict *d);
int dict_add(dict *d, const char *key, cc_obj *val);
cc_obj* dict_get(dict *d, const char *key);
int dict_delete(dict *d, const char *key);
void dict_resize(dict *d);
void dict_clear_expired(dict *d);
void dict_evict_lru_if_needed(dict *d);

#endif /* DICT_H */