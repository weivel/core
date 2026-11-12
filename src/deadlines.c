#include "deadlines.h"

#include <stdlib.h>
#include <string.h>

typedef struct deadline {
    int64_t at;
    weivel_handle subject;
} deadline;

struct weivel_deadlines {
    deadline *entries;
    size_t count;
    size_t capacity;
};

weivel_deadlines *weivel_deadlines_create(void) {
    return calloc(1, sizeof(weivel_deadlines));
}

void weivel_deadlines_destroy(weivel_deadlines *deadlines) {
    if (deadlines != NULL) {
        free(deadlines->entries);
        free(deadlines);
    }
}

static bool grow(weivel_deadlines *deadlines) {
    size_t capacity = deadlines->capacity == 0 ? 8 : deadlines->capacity * 2;
    deadline *entries = realloc(deadlines->entries, capacity * sizeof *entries);
    if (entries == NULL) {
        return false;
    }
    deadlines->entries = entries;
    deadlines->capacity = capacity;
    return true;
}

static size_t position_after_equal(const weivel_deadlines *deadlines, int64_t at) {
    size_t low = 0;
    size_t high = deadlines->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (deadlines->entries[middle].at <= at) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

weivel_status weivel_deadlines_add(weivel_deadlines *deadlines, int64_t at, weivel_handle subject) {
    if (deadlines->count == deadlines->capacity && !grow(deadlines)) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    size_t position = position_after_equal(deadlines, at);
    memmove(&deadlines->entries[position + 1], &deadlines->entries[position],
            (deadlines->count - position) * sizeof *deadlines->entries);
    deadlines->entries[position].at = at;
    deadlines->entries[position].subject = subject;
    deadlines->count++;
    return WEIVEL_STATUS_OK;
}

bool weivel_deadlines_cancel(weivel_deadlines *deadlines, weivel_handle subject) {
    size_t kept = 0;
    for (size_t index = 0; index < deadlines->count; index++) {
        if (deadlines->entries[index].subject != subject) {
            deadlines->entries[kept++] = deadlines->entries[index];
        }
    }
    bool removed = kept != deadlines->count;
    deadlines->count = kept;
    return removed;
}

bool weivel_deadlines_earliest(const weivel_deadlines *deadlines, int64_t *at) {
    if (deadlines->count == 0) {
        return false;
    }
    *at = deadlines->entries[0].at;
    return true;
}

bool weivel_deadlines_take_due(weivel_deadlines *deadlines, int64_t now, weivel_handle *subject) {
    if (deadlines->count == 0 || deadlines->entries[0].at > now) {
        return false;
    }
    *subject = deadlines->entries[0].subject;
    deadlines->count--;
    memmove(&deadlines->entries[0], &deadlines->entries[1], deadlines->count * sizeof *deadlines->entries);
    return true;
}
