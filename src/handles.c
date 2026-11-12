#include "handles.h"
#include "memory.h"

#include <stdbool.h>
#include <stdlib.h>

#define NO_FREE_SLOT UINT32_MAX

typedef struct handle_slot {
    uint32_t generation;
    uint32_t kind;
    uint32_t next_free;
    bool occupied;
    void *record;
} handle_slot;

struct weivel_handles {
    handle_slot *slots;
    uint32_t count;
    uint32_t capacity;
    uint32_t first_free;
    uint32_t live;
};

static uint32_t slot_of(weivel_handle handle) {
    return (uint32_t)(handle & 0xffffffffu);
}

static uint32_t generation_of(weivel_handle handle) {
    return (uint32_t)(handle >> 32);
}

static weivel_handle handle_for(uint32_t slot, uint32_t generation) {
    return ((weivel_handle)generation << 32) | slot;
}

weivel_handles *weivel_handles_create(void) {
    weivel_handles *handles = weivel_memory_allocate_zeroed(1, sizeof *handles);
    if (handles != NULL) {
        handles->first_free = NO_FREE_SLOT;
    }
    return handles;
}

void weivel_handles_destroy(weivel_handles *handles) {
    if (handles != NULL) {
        free(handles->slots);
        free(handles);
    }
}

uint32_t weivel_handles_next_generation(uint32_t generation) {
    return generation == UINT32_MAX ? 0 : generation + 1;
}

static bool grow(weivel_handles *handles) {
    if (handles->capacity >= NO_FREE_SLOT / 2) {
        return false;
    }
    uint32_t capacity = handles->capacity == 0 ? 16 : handles->capacity * 2;
    handle_slot *slots = weivel_memory_reallocate(handles->slots, (size_t)capacity * sizeof *slots);
    if (slots == NULL) {
        return false;
    }
    handles->slots = slots;
    handles->capacity = capacity;
    return true;
}

static bool take_slot(weivel_handles *handles, uint32_t *slot) {
    if (handles->first_free != NO_FREE_SLOT) {
        *slot = handles->first_free;
        handles->first_free = handles->slots[*slot].next_free;
        return true;
    }
    if (handles->count == handles->capacity && !grow(handles)) {
        return false;
    }
    *slot = handles->count++;
    handles->slots[*slot].generation = 1;
    return true;
}

weivel_status weivel_handles_allocate(weivel_handles *handles, uint32_t kind, void *record, weivel_handle *handle) {
    uint32_t slot;
    if (!take_slot(handles, &slot)) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    handle_slot *entry = &handles->slots[slot];
    entry->kind = kind;
    entry->occupied = true;
    entry->record = record;
    entry->next_free = NO_FREE_SLOT;
    *handle = handle_for(slot, entry->generation);
    handles->live++;
    return WEIVEL_STATUS_OK;
}

uint32_t weivel_handles_live(const weivel_handles *handles) {
    return handles->live;
}

uint32_t weivel_handles_slots(const weivel_handles *handles) {
    return handles->count;
}

static handle_slot *live_slot(const weivel_handles *handles, weivel_handle handle, uint32_t kind) {
    uint32_t slot = slot_of(handle);
    if (handle == 0 || slot >= handles->count) {
        return NULL;
    }
    handle_slot *entry = &handles->slots[slot];
    if (!entry->occupied || entry->generation != generation_of(handle) || entry->kind != kind) {
        return NULL;
    }
    return entry;
}

weivel_status weivel_handles_resolve(const weivel_handles *handles, weivel_handle handle, uint32_t kind,
                                     void **record) {
    handle_slot *entry = live_slot(handles, handle, kind);
    *record = entry == NULL ? NULL : entry->record;
    return entry == NULL ? WEIVEL_STATUS_REFUSED : WEIVEL_STATUS_OK;
}

weivel_status weivel_handles_release(weivel_handles *handles, weivel_handle handle, uint32_t kind) {
    handle_slot *entry = live_slot(handles, handle, kind);
    if (entry == NULL) {
        return WEIVEL_STATUS_REFUSED;
    }
    entry->occupied = false;
    entry->record = NULL;
    handles->live--;
    entry->generation = weivel_handles_next_generation(entry->generation);
    if (entry->generation != 0) {
        entry->next_free = handles->first_free;
        handles->first_free = slot_of(handle);
    }
    return WEIVEL_STATUS_OK;
}

void weivel_handles_unissue(weivel_handles *handles, weivel_handle handle, uint32_t kind) {
    handle_slot *entry = live_slot(handles, handle, kind);
    if (entry == NULL) {
        return;
    }
    uint32_t slot = slot_of(handle);
    entry->occupied = false;
    entry->record = NULL;
    handles->live--;
    if (entry->generation == 1 && slot == handles->count - 1) {
        handles->count--;
        return;
    }
    entry->next_free = handles->first_free;
    handles->first_free = slot;
}

weivel_handle weivel_handles_set_generation(weivel_handles *handles, weivel_handle handle, uint32_t kind,
                                            uint32_t generation) {
    handle_slot *entry = live_slot(handles, handle, kind);
    if (entry == NULL || generation == 0) {
        return 0;
    }
    entry->generation = generation;
    return handle_for(slot_of(handle), generation);
}
