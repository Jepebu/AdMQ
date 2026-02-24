#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "hash.h"

unsigned int hash(const char *key) {
    unsigned long int value = 0;
    unsigned int i = 0;
    unsigned int key_len = strlen(key);

    for (; i < key_len; ++i) {
        value = value * 37 + key[i];
    }
    return value % TABLE_SIZE;
}

HashTable *create_table() {
    HashTable *table = malloc(sizeof(HashTable));
    table->buckets = malloc(sizeof(Entry *) * TABLE_SIZE);
    for (int i = 0; i < TABLE_SIZE; i++) {
        table->buckets[i] = NULL;
    }
    return table;
}

void free_table(HashTable *table) {
    if (table == NULL) return;

    for (int i = 0; i < TABLE_SIZE; i++) {
        Entry *entry = table->buckets[i];
        while (entry != NULL) {
            Entry *next_entry = entry->next;
            free(entry->key);
            // We do NOT free(entry->value) here, as memory for the Client is managed externally
            free(entry);
            entry = next_entry;
        }
    }
    free(table->buckets);
    free(table);
}

void set(HashTable *table, const char *key, void *value) {
    unsigned int slot = hash(key);

    Entry *entry = table->buckets[slot];
    while (entry != NULL) {
        if (strcmp(entry->key, key) == 0) {
            entry->value = value; // Key exists, just update the pointer
            return;
        }
        entry = entry->next;
    }

    Entry *new_entry = malloc(sizeof(Entry));
    new_entry->key = strdup(key);
    new_entry->value = value;

    new_entry->next = table->buckets[slot];
    table->buckets[slot] = new_entry;
}

void *get(HashTable *table, const char *key) {
    unsigned int slot = hash(key);

    Entry *entry = table->buckets[slot];
    while (entry != NULL) {
        if (strcmp(entry->key, key) == 0) {
            return entry->value;
        }
        entry = entry->next;
    }
    return NULL; 
}

bool del(HashTable *table, const char *key) {
    if (table == NULL || key == NULL) return false;

    unsigned int slot = hash(key);
    Entry *current = table->buckets[slot];
    Entry *previous = NULL;

    while (current != NULL) {
        if (strcmp(current->key, key) == 0) {
            if (previous == NULL) {
                table->buckets[slot] = current->next;
            } else {
                previous->next = current->next;
            }

            free(current->key);
            free(current);
            return true;
        }
        previous = current;
        current = current->next;
    }
    return false;
}
