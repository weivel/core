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

/**
 * Allocates room ahead for `count` adds of keys not yet present, so those adds cannot run out of memory. On
 * WEIVEL_STATUS_OUT_OF_MEMORY the room made before the failure stays, harmless, until weivel_table_drop_spares.
 */
weivel_status weivel_table_reserve(weivel_table *table, uint32_t count);

/** Frees whatever room weivel_table_reserve made that no add used. */
void weivel_table_drop_spares(weivel_table *table);

/** Removes the key; false when it was not there. */
bool weivel_table_remove(weivel_table *table, const void *key, size_t length);

void *weivel_table_find_text(const weivel_table *table, const char *key);
weivel_status weivel_table_add_text(weivel_table *table, const char *key, void *value);
bool weivel_table_remove_text(weivel_table *table, const char *key);

#endif
