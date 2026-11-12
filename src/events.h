#ifndef WEIVEL_EVENTS_H
#define WEIVEL_EVENTS_H

#include <stdbool.h>
#include <weivel/core.h>

/** A first-in first-out queue of full-size events, each owning its `storage`. */
typedef struct weivel_events weivel_events;

weivel_events *weivel_events_create(uint32_t initial_capacity);

/** Frees the queue and the storage of every event still in it. */
void weivel_events_destroy(weivel_events *events);

uint32_t weivel_events_pending(const weivel_events *events);

/** Queues `event`, taking its `storage`; on failure the storage is freed, so the caller never frees it. */
weivel_status weivel_events_push(weivel_events *events, const weivel_event *event);

/** Moves the oldest event, storage and all, into `event`; false when the queue is empty. */
bool weivel_events_pop(weivel_events *events, weivel_event *event);

/**
 * Copies `count` strings into one allocation, the storage an event will own, and points each of `copies` at its
 * copy. A NULL text copies as an empty string. NULL when memory runs out.
 */
void *weivel_events_copy_texts(size_t count, const char *const *texts, const char **copies);

weivel_status weivel_events_push_log(weivel_events *events, uint32_t level, const char *text);
weivel_status weivel_events_push_refusal(weivel_events *events, uint32_t code, const char *detail, const char *fix);

#endif
