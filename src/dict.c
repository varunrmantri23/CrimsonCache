#define _POSIX_C_SOURCE 200809L
#include "dict.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

// simple hash function  -- djb2 //http://www.cse.yorku.ca/~oz/hash.html
static size_t dict_hash(const char *key) {
    size_t hash = 5381;
    int c;
    
    while ((c = *key++))
        hash = ((hash << 5) + hash) + c;
    
    return hash;
}

// get current time in milliseconds
static uint64_t current_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec) * 1000 + (tv.tv_usec / 1000);
}
//used for key expiration and LRU timestamp tracking


// create a new dictionary
dict* dict_create(size_t initial_size) {
    dict *d = malloc(sizeof(dict));
    if (!d) return NULL;
    
    if (initial_size < 4) initial_size = 4;
    
    // round up to power of 2
    size_t size = 1;
    while (size < initial_size) size *= 2;
    
    d->table = calloc(size, sizeof(dict_entry*));
    if (!d->table) {
        free(d);
        return NULL;
    }
    
    d->size = size;
    d->used = 0;
    d->mask = size - 1;
    d->max_memory = 0; // No limit by default
    d->used_memory = 0;
    
    return d;
}

// free the dictionary and all entries
void dict_free(dict *d) {
    if (!d) return;
    
    for (size_t i = 0; i < d->size; i++) {
        dict_entry *entry = d->table[i];
        while (entry) {
            dict_entry *next = entry->next;
            free(entry->key);
            
            // free value object
            if (entry->val) {
                free(entry->val->ptr);
                free(entry->val);
            }
            
            free(entry);
            entry = next;
        }
    }
    
    free(d->table);
    free(d);
}

// add or update a key-value pair
int dict_add(dict *d, const char *key, cc_obj *val) {
    if (!d || !key || !val) return 0;
    
    // check if we need to resize
    if (d->used >= d->size) {
        dict_resize(d);
    }
    
    // hash the key
    size_t idx = dict_hash(key) & d->mask;
    
    // check if key already exists
    dict_entry *entry = d->table[idx];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            // update existing entry
            // free old value
            if (entry->val) {
                d->used_memory -= entry->val->size;
                free(entry->val->ptr);
                free(entry->val);
            }
            
            // set new value
            entry->val = val;
            val->last_access = current_time_ms();
            d->used_memory += val->size;
            
            // check if we need to evict
            dict_evict_lru_if_needed(d);
            return 1;
        }
        entry = entry->next;
    }
    
    // create new entry
    entry = malloc(sizeof(dict_entry));
    if (!entry) return 0;
    
    entry->key = strdup(key);
    if (!entry->key) {
        free(entry);
        return 0;
    }
    
    entry->val = val;
    val->last_access = current_time_ms();
    d->used_memory += val->size;
    
    // add to hash table (prepend to list at idx)
    entry->next = d->table[idx];
    d->table[idx] = entry;
    d->used++;
    
    // check if we need to evict
    dict_evict_lru_if_needed(d);
    return 1;
}

// get a value by key
cc_obj* dict_get(dict *d, const char *key) {
    if (!d || !key) return NULL;
    
    // hash the key
    size_t idx = dict_hash(key) & d->mask;
    
    // search for the key
    dict_entry *entry = d->table[idx];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            // check if expired
            if (entry->val->expire != 0 && entry->val->expire < current_time_ms()) {
                // actually delete the expired key
                dict_delete(d, key);
                return NULL;
            }
            
            // update access time for LRU
            entry->val->last_access = current_time_ms();
            return entry->val;
        }
        entry = entry->next;
    }
    
    return NULL;
}

// delete a key
int dict_delete(dict *d, const char *key) {
    if (!d || !key) return 0;
    
    // hash the key
    size_t idx = dict_hash(key) & d->mask;
    
    dict_entry *entry = d->table[idx];
    dict_entry *prev = NULL;
    
    // search for the key
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            // found the key, remove it
            if (prev) {
                prev->next = entry->next;
            } else {
                d->table[idx] = entry->next;
            }
            
            // update used memory
            if (entry->val) {
                d->used_memory -= entry->val->size;
                free(entry->val->ptr);
                free(entry->val);
            }
            
            free(entry->key);
            free(entry);
            d->used--;
            return 1;
        }
        
        prev = entry;
        entry = entry->next;
    }
    
    return 0;
}

// resize the dictionary
void dict_resize(dict *d) {
    if (!d) return;
    
    size_t new_size = d->size * 2;
    dict_entry **new_table = calloc(new_size, sizeof(dict_entry*));
    if (!new_table) return;
    
    // rehash all entries
    for (size_t i = 0; i < d->size; i++) {
        dict_entry *entry = d->table[i];
        while (entry) {
            dict_entry *next = entry->next;
            
            // recalculate index with new size
            size_t idx = dict_hash(entry->key) & (new_size - 1);
            
            // move to new table
            entry->next = new_table[idx];
            new_table[idx] = entry;
            
            entry = next;
        }
    }
    
    // free old table and update dict
    free(d->table);
    d->table = new_table;
    d->size = new_size;
    d->mask = new_size - 1;
}

// clear expired keys
void dict_clear_expired(dict *d) {
    if (!d) return;
    
    uint64_t now = current_time_ms();
    
    for (size_t i = 0; i < d->size; i++) {
        dict_entry *entry = d->table[i];
        dict_entry *prev = NULL;
        
        while (entry) {
            if (entry->val->expire != 0 && entry->val->expire < now) {
                // this key has expired
                dict_entry *next = entry->next;
                
                if (prev) {
                    prev->next = next;
                } else {
                    d->table[i] = next;
                }
                
                // Update used memory
                d->used_memory -= entry->val->size;
                
                // Free memory
                free(entry->key);
                free(entry->val->ptr);
                free(entry->val);
                free(entry);
                
                d->used--;
                entry = next;
            } else {
                prev = entry;
                entry = entry->next;
            }
        }
    }
}

// find and evict the least recently used entry if needed
void dict_evict_lru_if_needed(dict *d) {
    if (!d || d->max_memory == 0 || d->used_memory <= d->max_memory) {
        return; // no need to evict
    }
    
    // find LRU entry
    dict_entry *lru_entry = NULL;
    dict_entry *lru_prev = NULL;
    size_t lru_idx = 0;
    uint64_t oldest_access = UINT64_MAX;
    
    for (size_t i = 0; i < d->size; i++) {
        dict_entry *entry = d->table[i];
        dict_entry *prev = NULL;
        
        while (entry) {
            if (entry->val->last_access < oldest_access) {
                oldest_access = entry->val->last_access;
                lru_entry = entry;
                lru_prev = prev;
                lru_idx = i;
            }
            prev = entry;
            entry = entry->next;
        }
    }
    
    // remove LRU entry if found
    if (lru_entry) {
        if (lru_prev) {
            lru_prev->next = lru_entry->next;
        } else {
            d->table[lru_idx] = lru_entry->next;
        }
        
        // update used memory
        d->used_memory -= lru_entry->val->size;
        
        // free memory
        free(lru_entry->key);
        free(lru_entry->val->ptr);
        free(lru_entry->val);
        free(lru_entry);
        
        d->used--;
        
        // recursively evict if still over limit
        dict_evict_lru_if_needed(d);
    }
}