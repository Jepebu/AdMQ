#ifndef HASH_H
#define HASH_H

#include <stdbool.h>

#define TABLE_SIZE 100

// A node representing a key-value pair
typedef struct Entry {
    char *key;
    void *value;        // Updated to void* to generically store Client struct pointers
    struct Entry *next; // Pointer to the next entry (for collisions)
} Entry;

// The Hash Table structure
typedef struct HashTable {
    Entry **buckets; // Array of pointers to Entries
} HashTable;

unsigned int hash(const char *key);
HashTable *create_table();
void free_table(HashTable *table);
void set(HashTable *table, const char *key, void *value);
void *get(HashTable *table, const char *key);
bool del(HashTable *table, const char *key);

#endif
