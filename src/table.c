#include "table.h"
#include "memory.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FIRST_CAPACITY 16u

typedef struct table_entry {
    const void *key;
    size_t length;
    uint64_t hash;
    void *value;
    struct table_entry *next;
} table_entry;

struct weivel_table {
    table_entry **buckets;
    uint32_t capacity;
    uint32_t count;
    /** Entries allocated ahead by weivel_table_reserve, chained by `next`, so an add that uses one cannot fail. */
    table_entry *spares;
};

static uint64_t hash_of(const void *key, size_t length) {
    const unsigned char *bytes = key;
    uint64_t hash = 14695981039346656037u;
    for (size_t index = 0; index < length; index++) {
        hash = (hash ^ bytes[index]) * 1099511628211u;
    }
    return hash;
}

weivel_table *weivel_table_create(void) {
    weivel_table *table = weivel_memory_allocate_zeroed(1, sizeof *table);
    if (table == NULL) {
        return NULL;
    }
    table->buckets = weivel_memory_allocate_zeroed(FIRST_CAPACITY, sizeof *table->buckets);
    if (table->buckets == NULL) {
        free(table);
        return NULL;
    }
    table->capacity = FIRST_CAPACITY;
    return table;
}

void weivel_table_destroy(weivel_table *table) {
    if (table == NULL) {
        return;
    }
    weivel_table_drop_spares(table);
    for (uint32_t bucket = 0; bucket < table->capacity; bucket++) {
        table_entry *entry = table->buckets[bucket];
        while (entry != NULL) {
            table_entry *next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(table->buckets);
    free(table);
}

static table_entry **place_of(const weivel_table *table, const void *key, size_t length, uint64_t hash) {
    table_entry **place = &table->buckets[hash & (table->capacity - 1)];
    while (*place != NULL) {
        table_entry *entry = *place;
        if (entry->hash == hash && entry->length == length && memcmp(entry->key, key, length) == 0) {
            return place;
        }
        place = &entry->next;
    }
    return place;
}

void *weivel_table_find(const weivel_table *table, const void *key, size_t length) {
    table_entry *entry = *place_of(table, key, length, hash_of(key, length));
    return entry == NULL ? NULL : entry->value;
}

/**
 * Doubles the buckets, which only shortens chains, so a table that cannot grow stays correct, just slower.
 * @implNote The guard keeps `capacity` a power of two that fits a uint32_t, which `hash & (capacity - 1)` relies on
 * for every bucket index; past it the table stops growing rather than wrapping to 0.
 */
static void grow(weivel_table *table) {
    if (table->capacity > UINT32_MAX / 2) {
        return;
    }
    uint32_t capacity = table->capacity * 2;
    table_entry **buckets = weivel_memory_allocate_zeroed(capacity, sizeof *buckets);
    if (buckets == NULL) {
        return;
    }
    for (uint32_t bucket = 0; bucket < table->capacity; bucket++) {
        table_entry *entry = table->buckets[bucket];
        while (entry != NULL) {
            table_entry *next = entry->next;
            table_entry **head = &buckets[entry->hash & (capacity - 1)];
            entry->next = *head;
            *head = entry;
            entry = next;
        }
    }
    free(table->buckets);
    table->buckets = buckets;
    table->capacity = capacity;
}

weivel_status weivel_table_add(weivel_table *table, const void *key, size_t length, void *value) {
    if (table->count >= table->capacity) {
        grow(table);
    }
    uint64_t hash = hash_of(key, length);
    table_entry **place = place_of(table, key, length, hash);
    if (*place != NULL) {
        (*place)->value = value;
        return WEIVEL_STATUS_OK;
    }
    table_entry *entry = table->spares;
    if (entry != NULL) {
        table->spares = entry->next;
    } else if ((entry = weivel_memory_allocate(sizeof *entry)) == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    entry->key = key;
    entry->length = length;
    entry->hash = hash;
    entry->value = value;
    entry->next = NULL;
    *place = entry;
    table->count++;
    return WEIVEL_STATUS_OK;
}

bool weivel_table_remove(weivel_table *table, const void *key, size_t length) {
    table_entry **place = place_of(table, key, length, hash_of(key, length));
    table_entry *entry = *place;
    if (entry == NULL) {
        return false;
    }
    *place = entry->next;
    free(entry);
    table->count--;
    return true;
}

weivel_status weivel_table_reserve(weivel_table *table, uint32_t count) {
    for (uint32_t index = 0; index < count; index++) {
        table_entry *entry = weivel_memory_allocate(sizeof *entry);
        if (entry == NULL) {
            return WEIVEL_STATUS_OUT_OF_MEMORY;
        }
        entry->next = table->spares;
        table->spares = entry;
    }
    return WEIVEL_STATUS_OK;
}

void weivel_table_drop_spares(weivel_table *table) {
    while (table->spares != NULL) {
        table_entry *next = table->spares->next;
        free(table->spares);
        table->spares = next;
    }
}

void *weivel_table_find_text(const weivel_table *table, const char *key) {
    return weivel_table_find(table, key, strlen(key));
}

weivel_status weivel_table_add_text(weivel_table *table, const char *key, void *value) {
    return weivel_table_add(table, key, strlen(key), value);
}

bool weivel_table_remove_text(weivel_table *table, const char *key) {
    return weivel_table_remove(table, key, strlen(key));
}
