#include "events.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_CAPACITY 16u

struct weivel_events {
    weivel_event *ring;
    uint32_t capacity;
    uint32_t first;
    uint32_t count;
};

weivel_events *weivel_events_create(uint32_t initial_capacity) {
    weivel_events *events = calloc(1, sizeof *events);
    if (events == NULL) {
        return NULL;
    }
    events->capacity = initial_capacity == 0 ? DEFAULT_CAPACITY : initial_capacity;
    events->ring = calloc(events->capacity, sizeof *events->ring);
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

static bool grow(weivel_events *events) {
    if (events->capacity > UINT32_MAX / 2) {
        return false;
    }
    uint32_t capacity = events->capacity * 2;
    weivel_event *ring = malloc((size_t)capacity * sizeof *ring);
    if (ring == NULL) {
        return false;
    }
    for (uint32_t index = 0; index < events->count; index++) {
        ring[index] = events->ring[(events->first + index) % events->capacity];
    }
    free(events->ring);
    events->ring = ring;
    events->capacity = capacity;
    events->first = 0;
    return true;
}

weivel_status weivel_events_push(weivel_events *events, const weivel_event *event) {
    if (events->count == events->capacity && !grow(events)) {
        free(event->storage);
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_event *slot = &events->ring[(events->first + events->count) % events->capacity];
    *slot = *event;
    slot->size = (uint32_t)sizeof *slot;
    events->count++;
    return WEIVEL_STATUS_OK;
}

bool weivel_events_pop(weivel_events *events, weivel_event *event) {
    if (events->count == 0) {
        return false;
    }
    *event = events->ring[events->first];
    events->first = (events->first + 1) % events->capacity;
    events->count--;
    return true;
}

void *weivel_events_copy_texts(size_t count, const char *const *texts, const char **copies) {
    size_t total = 0;
    for (size_t index = 0; index < count; index++) {
        total += (texts[index] == NULL ? 0 : strlen(texts[index])) + 1;
    }
    char *storage = malloc(total == 0 ? 1 : total);
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
    return weivel_events_push(events, &event);
}

weivel_status weivel_events_push_refusal(weivel_events *events, uint32_t code, const char *detail, const char *fix) {
    weivel_event event = empty_event(WEIVEL_EVENT_REFUSAL);
    const char *texts[2] = {detail, fix};
    const char *copies[2];
    event.data.refusal.code = code;
    event.storage = weivel_events_copy_texts(2, texts, copies);
    if (event.storage == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    event.data.refusal.detail = copies[0];
    event.data.refusal.fix = copies[1];
    return weivel_events_push(events, &event);
}

weivel_status weivel_events_push_want(weivel_events *events, const weivel_want_event *settled, const char *detail,
                                      const char *fix) {
    weivel_event event = empty_event(WEIVEL_EVENT_WANT_SETTLED);
    event.data.want = *settled;
    event.data.want.refusal.detail = NULL;
    event.data.want.refusal.fix = NULL;
    if (settled->implementation != 0) {
        event.data.want.refusal.code = WEIVEL_REFUSAL_NONE;
        return weivel_events_push(events, &event);
    }
    const char *texts[2] = {detail, fix};
    const char *copies[2];
    event.storage = weivel_events_copy_texts(2, texts, copies);
    if (event.storage == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    event.data.want.refusal.detail = copies[0];
    event.data.want.refusal.fix = copies[1];
    return weivel_events_push(events, &event);
}

weivel_status weivel_events_push_watch(weivel_events *events, const weivel_watch_event *heard) {
    weivel_event event = empty_event(WEIVEL_EVENT_WATCH);
    event.data.watch = *heard;
    return weivel_events_push(events, &event);
}

weivel_status weivel_events_push_revoked(weivel_events *events, const weivel_revoked_event *revoked) {
    weivel_event event = empty_event(WEIVEL_EVENT_REVOKED);
    event.data.revoked = *revoked;
    return weivel_events_push(events, &event);
}

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
    return weivel_events_push(events, &event);
}
