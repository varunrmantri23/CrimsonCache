#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <stdio.h>  // Required for FILE type
#include "dict.h"

// rdb file header constants
#define RDB_MAGIC "CCDB"  // crimsoncache database 
#define RDB_VERSION 1

// commands related to persistence
#define RDB_SET 1   // string data 
#define RDB_END 255 // end of file marker

// function prototypes
int save_rdb_to_file(dict *db, const char *filename);
int load_rdb_from_file(dict *db, const char *filename);
int background_save(dict *db, const char *filename);

// helper functions
int rdb_save_string(FILE *fp, const char *str, size_t len);
int rdb_load_string(FILE *fp, char **str, size_t *len);

#endif /* PERSISTENCE_H */