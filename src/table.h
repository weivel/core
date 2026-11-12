#ifndef WEIVEL_TABLE_H
#define WEIVEL_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <weivel/core.h>

/**
 * A hash table from byte keys to records. Keys are borrowed: each must stay unchanged in memory for as long as its
 * entry is in the table, which is why every key the core uses lives inside the record it maps to.
 */
typedef struct weivel_table weivel_table;

weivel_table *weivel_table_create(void);

/** Frees the table, never its keys or values. */
void weivel_table_destroy(weivel_table *table);

/** The value under the key, or NULL. */
void *weivel_table_find(const weivel_table *table, const void *key, size_t length);

/** Puts a value under a key, replacing any value already there. */
weivel_status weivel_table_add(weivel_table *table, const void *key, size_t length, void *value);

/** Removes the key; false when it was not there. */
bool weivel_table_remove(weivel_table *table, const void *key, size_t length);

void *weivel_table_find_text(const weivel_table *table, const char *key);
weivel_status weivel_table_add_text(weivel_table *table, const char *key, void *value);
bool weivel_table_remove_text(weivel_table *table, const char *key);

#endif
