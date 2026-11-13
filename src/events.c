#include "events.h"
#include "memory.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_CAPACITY 16u

struct weivel_events {
    weivel_event *ring;
    uint32_t capacity;
    uint32_t first;
    uint32_t count;
    /** Events owed and not yet delivered; `capacity` always covers `count` plus these. */
    uint32_t reserved;
    /** Whether a deadline announcement waits in the ring; while none does, `capacity` also keeps room for one. */
    bool deadline_waiting;
};

weivel_events *weivel_events_create(uint32_t initial_capacity) {
    weivel_events *events = weivel_memory_allocate_zeroed(1, sizeof *events);
    if (events == NULL) {
        return NULL;
    }
    events->capacity = initial_capacity == 0 ? DEFAULT_CAPACITY : initial_capacity;
    events->ring = weivel_memory_allocate_zeroed(events->capacity, sizeof *events->ring);
    if (events->ring == NULL) {
        free(events);
        return NULL;
    }
    return events;
}

void weivel_events_destroy(weivel_events *events) {
    if (events == NULL) {
        return;
    }
    weivel_event event;
    while (weivel_events_pop(events, &event)) {
        free(event.storage);
    }
    free(events->ring);
    free(events);
}

uint32_t weivel_events_pending(const weivel_events *events) {
    return events->count;
}

/** The room spoken for: every event queued, every reservation, and the deadline announcement's when none waits. */
static uint64_t spoken_for(const weivel_events *events) {
    return (uint64_t)events->count + events->reserved + (events->deadline_waiting ? 0u : 1u);
}

/** Grows the ring by doubling until it holds `needed` events; false, with nothing changed, when it cannot. */
static bool grow_to(weivel_events *events, uint64_t needed) {
    uint64_t capacity = events->capacity;
    while (capacity < needed) {
        capacity *= 2;
    }
    if (capacity == events->capacity) {
        return true;
    }
    if (capacity > UINT32_MAX) {
        return false;
    }
    weivel_event *ring = weivel_memory_allocate((size_t)capacity * sizeof *ring);
    if (ring == NULL) {
        return false;
    }
    for (uint32_t index = 0; index < events->count; index++) {
        ring[index] = events->ring[(events->first + index) % events->capacity];
    }
    free(events->ring);
    events->ring = ring;
    events->capacity = (uint32_t)capacity;
    events->first = 0;
    return true;
}

weivel_status weivel_events_reserve(weivel_events *events, uint32_t count) {
    if (!grow_to(events, spoken_for(events) + count)) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    events->reserved += count;
    return WEIVEL_STATUS_OK;
}

void weivel_events_unreserve(weivel_events *events, uint32_t count) {
    events->reserved -= count;
}

uint32_t weivel_events_reserved(const weivel_events *events) {
    return events->reserved;
}

static void place(weivel_events *events, const weivel_event *event) {
    weivel_event *slot = &events->ring[(events->first + events->count) % events->capacity];
    *slot = *event;
    slot->size = (uint32_t)sizeof *slot;
    events->count++;
}

/** Queues an unreserved event, taking its `storage`; on failure the storage is freed, so the caller never frees it. */
static weivel_status push(weivel_events *events, const weivel_event *event) {
    if (!grow_to(events, spoken_for(events) + 1)) {
        free(event->storage);
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    place(events, event);
    return WEIVEL_STATUS_OK;
}

static void deliver(weivel_events *events, const weivel_event *event) {
    events->reserved--;
    place(events, event);
}

void weivel_events_drop_newest(weivel_events *events) {
    events->count--;
    weivel_event *newest = &events->ring[(events->first + events->count) % events->capacity];
    if (newest->kind == WEIVEL_EVENT_DEADLINE) {
        events->deadline_waiting = false;
    }
    free(newest->storage);
    memset(newest, 0, sizeof *newest);
}

bool weivel_events_pop(weivel_events *events, weivel_event *event) {
    if (events->count == 0) {
        return false;
    }
    *event = events->ring[events->first];
    events->first = (events->first + 1) % events->capacity;
    events->count--;
    if (event->kind == WEIVEL_EVENT_DEADLINE) {
        events->deadline_waiting = false;
    }
    return true;
}

static weivel_event *at_position(weivel_events *events, uint32_t position) {
    return &events->ring[(events->first + position) % events->capacity];
}

/** Takes the waiting deadline announcement out of the ring, closing the gap, so its room is free again. */
static void take_out_waiting_deadline(weivel_events *events) {
    uint32_t position = 0;
    while (at_position(events, position)->kind != WEIVEL_EVENT_DEADLINE) {
        position++;
    }
    for (; position + 1 < events->count; position++) {
        *at_position(events, position) = *at_position(events, position + 1);
    }
    events->count--;
    events->deadline_waiting = false;
}

void *weivel_events_copy_texts(size_t count, const char *const *texts, const char **copies) {
    size_t total = 0;
    for (size_t index = 0; index < count; index++) {
        total += (texts[index] == NULL ? 0 : strlen(texts[index])) + 1;
    }
    char *storage = weivel_memory_allocate(total == 0 ? 1 : total);
    if (storage == NULL) {
        return NULL;
    }
    char *cursor = storage;
    for (size_t index = 0; index < count; index++) {
        size_t length = texts[index] == NULL ? 0 : strlen(texts[index]);
        if (length > 0) {
            memcpy(cursor, texts[index], length);
        }
        cursor[length] = '\0';
        copies[index] = cursor;
        cursor += length + 1;
    }
    return storage;
}

void *weivel_events_copy_refusal(const char *subject, const char *detail, const char *fix, const char **copies) {
    const char *texts[3] = {subject, detail, fix};
    void *storage = weivel_events_copy_texts(3, texts, copies);
    if (storage != NULL && subject == NULL) {
        copies[0] = NULL;
    }
    return storage;
}

uint32_t weivel_events_has_fix(const char *fix) {
    return fix != NULL && fix[0] != '\0' ? 1u : 0u;
}

static weivel_event empty_event(uint32_t kind) {
    weivel_event event;
    memset(&event, 0, sizeof event);
    event.size = (uint32_t)sizeof event;
    event.kind = kind;
    return event;
}

weivel_status weivel_events_push_log(weivel_events *events, uint32_t level, const char *text) {
    weivel_event event = empty_event(WEIVEL_EVENT_LOG);
    event.data.log.level = level;
    event.storage = weivel_events_copy_texts(1, &text, &event.data.log.text);
    if (event.storage == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    return push(events, &event);
}

_Static_assert(sizeof(weivel_refusal_event) <= WEIVEL_EVENT_PAYLOAD_WORDS * sizeof(uint64_t),
               "a refusal event must fit the event's payload union");

weivel_status weivel_events_push_refusal(weivel_events *events, uint32_t code, const char *subject, const char *detail,
                                         const char *fix) {
    weivel_event event = empty_event(WEIVEL_EVENT_REFUSAL);
    const char *copies[3];
    event.storage = weivel_events_copy_refusal(subject, detail, fix, copies);
    if (event.storage == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    event.data.refused.refusal.code = code;
    event.data.refused.refusal.detail = copies[1];
    event.data.refused.refusal.fix = copies[2];
    event.data.refused.subject = copies[0];
    event.data.refused.has_fix = weivel_events_has_fix(fix);
    return push(events, &event);
}

/**
 * What a rejected want carries when memory runs out copying its refusal's texts.
 * @implNote Static, so `storage` stays NULL and the release frees nothing it did not allocate.
 */
static const char *const rejected_without_detail = "memory ran out describing why this want was rejected";
static const char *const rejected_without_fix = "read the refusal code, which is exact";

_Static_assert(sizeof(weivel_want_event) <= WEIVEL_EVENT_PAYLOAD_WORDS * sizeof(uint64_t),
               "a want event must fit the event's payload union");

/** Copies a settlement's one text into storage the event owns; NULL, with the event carrying none, when it cannot. */
static void keep_subject(weivel_event *event, const char *subject) {
    if (subject != NULL) {
        event->storage = weivel_events_copy_texts(1, &subject, &event->data.want.subject);
        if (event->storage == NULL) {
            event->data.want.subject = NULL;
        }
    }
}

void weivel_events_deliver_want(weivel_events *events, const weivel_want_event *settled, const char *subject,
                                const char *detail, const char *fix) {
    weivel_event event = empty_event(WEIVEL_EVENT_WANT_SETTLED);
    event.data.want = *settled;
    event.data.want.refusal.detail = NULL;
    event.data.want.refusal.fix = NULL;
    event.data.want.subject = NULL;
    event.data.want.has_fix = 0;
    if (settled->implementation != 0) {
        event.data.want.refusal.code = WEIVEL_REFUSAL_NONE;
        keep_subject(&event, subject);
        deliver(events, &event);
        return;
    }
    const char *copies[3] = {NULL, rejected_without_detail, rejected_without_fix};
    event.storage = weivel_events_copy_refusal(subject, detail, fix, copies);
    event.data.want.subject = copies[0];
    event.data.want.refusal.detail = copies[1];
    event.data.want.refusal.fix = copies[2];
    event.data.want.has_fix = weivel_events_has_fix(event.data.want.refusal.fix);
    deliver(events, &event);
}

void weivel_events_deliver_watch(weivel_events *events, const weivel_watch_event *heard) {
    weivel_event event = empty_event(WEIVEL_EVENT_WATCH);
    event.data.watch = *heard;
    deliver(events, &event);
}

void weivel_events_deliver_revoked(weivel_events *events, const weivel_revoked_event *revoked) {
    weivel_event event = empty_event(WEIVEL_EVENT_REVOKED);
    event.data.revoked = *revoked;
    deliver(events, &event);
}

_Static_assert(sizeof(weivel_capability_event) <= WEIVEL_EVENT_PAYLOAD_WORDS * sizeof(uint64_t),
               "a capability event must fit the event's payload union");

void weivel_events_deliver_capability(weivel_events *events, const weivel_capability_event *heard) {
    weivel_event event = empty_event(WEIVEL_EVENT_CAPABILITY);
    event.data.capability = *heard;
    deliver(events, &event);
}

/** Copies core-owned numbers, at today's size, into one allocation an event will own; NULL when memory runs out. */
static weivel_number *copy_numbers(const weivel_numbers *numbers) {
    size_t bytes = (size_t)numbers->count * sizeof(weivel_number);
    for (uint32_t index = 0; index < numbers->count; index++) {
        bytes += strlen(numbers->items[index].name) + 1;
    }
    weivel_number *items = weivel_memory_allocate(bytes);
    if (items == NULL) {
        return NULL;
    }
    char *cursor = (char *)items + (size_t)numbers->count * sizeof(weivel_number);
    for (uint32_t index = 0; index < numbers->count; index++) {
        size_t length = strlen(numbers->items[index].name) + 1;
        memcpy(cursor, numbers->items[index].name, length);
        items[index].size = (uint32_t)sizeof items[index];
        items[index].name = cursor;
        items[index].value = numbers->items[index].value;
        cursor += length;
    }
    return items;
}

_Static_assert(sizeof(weivel_capacity_event) <= WEIVEL_EVENT_PAYLOAD_WORDS * sizeof(uint64_t),
               "a capacity event must fit the event's payload union");

weivel_status weivel_events_push_capacity(weivel_events *events, const weivel_capacity_event *asked,
                                          const weivel_numbers *constraints) {
    weivel_event event = empty_event(WEIVEL_EVENT_CAPACITY_WANTED);
    event.data.capacity = *asked;
    event.data.capacity.constraints.items = NULL;
    event.data.capacity.constraints.count = 0;
    if (constraints != NULL && constraints->count > 0) {
        weivel_number *items = copy_numbers(constraints);
        if (items == NULL) {
            return WEIVEL_STATUS_OUT_OF_MEMORY;
        }
        event.storage = items;
        event.data.capacity.constraints.items = items;
        event.data.capacity.constraints.count = constraints->count;
    }
    return push(events, &event);
}

_Static_assert(sizeof(weivel_deadline_event) <= WEIVEL_EVENT_PAYLOAD_WORDS * sizeof(uint64_t),
               "a deadline event must fit the event's payload union");

void weivel_events_announce_deadline(weivel_events *events, const weivel_deadline_event *deadline) {
    if (events->deadline_waiting) {
        take_out_waiting_deadline(events);
    }
    weivel_event event = empty_event(WEIVEL_EVENT_DEADLINE);
    event.data.deadline = *deadline;
    place(events, &event);
    events->deadline_waiting = true;
}

_Static_assert(sizeof(weivel_reach_refusal) <= WEIVEL_EVENT_PAYLOAD_WORDS * sizeof(uint64_t),
               "a reach refusal must fit the event's payload union");

weivel_status weivel_events_push_reach(weivel_events *events, uint32_t kind, weivel_handle plugin, const char *id,
                                       uint32_t code, const char *detail, const char *fix) {
    weivel_event event = empty_event(WEIVEL_EVENT_REACH_REFUSED);
    const char *texts[3] = {id, detail, fix};
    const char *copies[3];
    event.storage = weivel_events_copy_texts(3, texts, copies);
    if (event.storage == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    event.data.reach.kind = kind;
    event.data.reach.plugin = plugin;
    event.data.reach.id = copies[0];
    event.data.reach.refusal.code = code;
    event.data.reach.refusal.detail = copies[1];
    event.data.reach.refusal.fix = copies[2];
    event.data.reach.subject = copies[0];
    event.data.reach.has_fix = weivel_events_has_fix(fix);
    return push(events, &event);
}
