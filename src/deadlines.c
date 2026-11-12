#include "deadlines.h"

#include <stdlib.h>

#include "table.h"

/** One deadline. `position` is its place in the heap, kept current on every move so it can be cancelled there. */
typedef struct deadline {
    int64_t at;
    uint64_t order;
    weivel_handle subject;
    size_t position;
} deadline;

/**
 * A binary min-heap ordered by time, then by the order deadlines were added, with an index from each subject to its
 * deadline, so adding, cancelling and taking the earliest each cost O(log n).
 */
struct weivel_deadlines {
    deadline **heap;
    size_t count;
    size_t capacity;
    uint64_t next_order;
    weivel_table *by_subject;
};

weivel_deadlines *weivel_deadlines_create(void) {
    weivel_deadlines *deadlines = calloc(1, sizeof *deadlines);
    if (deadlines == NULL) {
        return NULL;
    }
    deadlines->by_subject = weivel_table_create();
    if (deadlines->by_subject == NULL) {
        free(deadlines);
        return NULL;
    }
    return deadlines;
}

void weivel_deadlines_destroy(weivel_deadlines *deadlines) {
    if (deadlines == NULL) {
        return;
    }
    for (size_t index = 0; index < deadlines->count; index++) {
        free(deadlines->heap[index]);
    }
    free(deadlines->heap);
    weivel_table_destroy(deadlines->by_subject);
    free(deadlines);
}

size_t weivel_deadlines_count(const weivel_deadlines *deadlines) {
    return deadlines->count;
}

static bool earlier(const deadline *left, const deadline *right) {
    return left->at != right->at ? left->at < right->at : left->order < right->order;
}

static void place(weivel_deadlines *deadlines, deadline *entry, size_t position) {
    deadlines->heap[position] = entry;
    entry->position = position;
}

static void rise(weivel_deadlines *deadlines, deadline *entry) {
    size_t position = entry->position;
    while (position > 0) {
        size_t parent = (position - 1) / 2;
        if (!earlier(entry, deadlines->heap[parent])) {
            break;
        }
        place(deadlines, deadlines->heap[parent], position);
        position = parent;
    }
    place(deadlines, entry, position);
}

static void sink(weivel_deadlines *deadlines, deadline *entry) {
    size_t position = entry->position;
    for (;;) {
        size_t child = 2 * position + 1;
        if (child >= deadlines->count) {
            break;
        }
        if (child + 1 < deadlines->count && earlier(deadlines->heap[child + 1], deadlines->heap[child])) {
            child++;
        }
        if (!earlier(deadlines->heap[child], entry)) {
            break;
        }
        place(deadlines, deadlines->heap[child], position);
        position = child;
    }
    place(deadlines, entry, position);
}

static bool grow(weivel_deadlines *deadlines) {
    size_t capacity = deadlines->capacity == 0 ? 8 : deadlines->capacity * 2;
    deadline **heap = realloc(deadlines->heap, capacity * sizeof *heap);
    if (heap == NULL) {
        return false;
    }
    deadlines->heap = heap;
    deadlines->capacity = capacity;
    return true;
}

/** Takes a deadline out of the heap and the index, moving the last one into its place, and frees it. */
static void remove_entry(weivel_deadlines *deadlines, deadline *entry) {
    weivel_table_remove(deadlines->by_subject, &entry->subject, sizeof entry->subject);
    deadline *last = deadlines->heap[--deadlines->count];
    if (last != entry) {
        place(deadlines, last, entry->position);
        rise(deadlines, last);
        sink(deadlines, last);
    }
    free(entry);
}

/** Moves an existing deadline to `at`, as though added now, which needs no memory and so cannot fail. */
static void move_entry(weivel_deadlines *deadlines, deadline *entry, int64_t at) {
    entry->at = at;
    entry->order = deadlines->next_order++;
    rise(deadlines, entry);
    sink(deadlines, entry);
}

weivel_status weivel_deadlines_add(weivel_deadlines *deadlines, int64_t at, weivel_handle subject) {
    deadline *existing = weivel_table_find(deadlines->by_subject, &subject, sizeof subject);
    if (existing != NULL) {
        move_entry(deadlines, existing, at);
        return WEIVEL_STATUS_OK;
    }
    if (deadlines->count == deadlines->capacity && !grow(deadlines)) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    deadline *entry = malloc(sizeof *entry);
    if (entry == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    entry->at = at;
    entry->order = deadlines->next_order++;
    entry->subject = subject;
    if (weivel_table_add(deadlines->by_subject, &entry->subject, sizeof entry->subject, entry) != WEIVEL_STATUS_OK) {
        free(entry);
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    place(deadlines, entry, deadlines->count++);
    rise(deadlines, entry);
    return WEIVEL_STATUS_OK;
}

bool weivel_deadlines_cancel(weivel_deadlines *deadlines, weivel_handle subject) {
    deadline *entry = weivel_table_find(deadlines->by_subject, &subject, sizeof subject);
    if (entry == NULL) {
        return false;
    }
    remove_entry(deadlines, entry);
    return true;
}

bool weivel_deadlines_earliest(const weivel_deadlines *deadlines, int64_t *at) {
    if (deadlines->count == 0) {
        return false;
    }
    *at = deadlines->heap[0]->at;
    return true;
}

bool weivel_deadlines_next_due(const weivel_deadlines *deadlines, int64_t now, weivel_handle *subject) {
    if (deadlines->count == 0 || deadlines->heap[0]->at > now) {
        return false;
    }
    *subject = deadlines->heap[0]->subject;
    return true;
}

bool weivel_deadlines_take_due(weivel_deadlines *deadlines, int64_t now, weivel_handle *subject) {
    if (!weivel_deadlines_next_due(deadlines, now, subject)) {
        return false;
    }
    remove_entry(deadlines, deadlines->heap[0]);
    return true;
}
