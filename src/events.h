#ifndef WEIVEL_EVENTS_H
#define WEIVEL_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <weivel/core.h>

/** A first-in first-out queue of full-size events, each owning its `storage`. */
typedef struct weivel_events weivel_events;

weivel_events *weivel_events_create(uint32_t initial_capacity);

/** Frees the queue and the storage of every event still in it. */
void weivel_events_destroy(weivel_events *events);

uint32_t weivel_events_pending(const weivel_events *events);

/**
 * Makes room for `count` events the core owes and will deliver later: the queue then always has room for every
 * reservation outstanding, so delivering a reserved event can never fail. On WEIVEL_STATUS_OUT_OF_MEMORY nothing
 * is reserved.
 */
weivel_status weivel_events_reserve(weivel_events *events, uint32_t count);

/** Gives back `count` reservations that will never be delivered. */
void weivel_events_unreserve(weivel_events *events, uint32_t count);

/** How many reservations are outstanding. */
uint32_t weivel_events_reserved(const weivel_events *events);

/** Moves the oldest event, storage and all, into `event`; false when the queue is empty. */
bool weivel_events_pop(weivel_events *events, weivel_event *event);

/** Frees the newest event, taking back an event queued by a change that is being undone. */
void weivel_events_drop_newest(weivel_events *events);

/**
 * Copies `count` strings into one allocation, the storage an event will own, and points each of `copies` at its
 * copy. A NULL text copies as an empty string. NULL when memory runs out.
 */
void *weivel_events_copy_texts(size_t count, const char *const *texts, const char **copies);

/**
 * Copies a refusal's subject, detail and fix, in that order, as weivel_events_copy_texts does, except that a NULL
 * subject stays NULL.
 */
void *weivel_events_copy_refusal(const char *subject, const char *detail, const char *fix, const char **copies);

/** A refusal's `has_fix`: 1 when `fix` is non-empty, else 0. */
uint32_t weivel_events_has_fix(const char *fix);

/**
 * Queues an event nothing reserved, in room no reservation needs; each answers WEIVEL_STATUS_OUT_OF_MEMORY, with
 * nothing queued, when memory runs out.
 */
weivel_status weivel_events_push_log(weivel_events *events, uint32_t level, const char *text);
weivel_status weivel_events_push_refusal(weivel_events *events, uint32_t code, const char *subject, const char *detail,
                                         const char *fix);

/** Queues a capacity question, copying `constraints`, which may be NULL for none, into the event's storage. */
weivel_status weivel_events_push_capacity(weivel_events *events, const weivel_capacity_event *asked,
                                          const weivel_numbers *constraints);

/** Queues a reported refusal of reach, copying `id` and both texts. */
weivel_status weivel_events_push_reach(weivel_events *events, uint32_t kind, weivel_handle plugin, const char *id,
                                       uint32_t code, const char *detail, const char *fix);

/**
 * Delivers an event a reservation was made for, consuming it; none can fail. A want's settlement carries its
 * service when `implementation` is not 0, else the refusal given, whose texts become a fixed sentence and whose
 * subject is lost when memory runs out copying them, so the event itself is never lost. `subject` is the rejection's,
 * or a degraded settlement's constraint, NULL otherwise.
 */
void weivel_events_deliver_want(weivel_events *events, const weivel_want_event *settled, const char *subject,
                                const char *detail, const char *fix);
void weivel_events_deliver_watch(weivel_events *events, const weivel_watch_event *heard);
void weivel_events_deliver_revoked(weivel_events *events, const weivel_revoked_event *revoked);
void weivel_events_deliver_capability(weivel_events *events, const weivel_capability_event *heard);

/**
 * Queues the core's earliest deadline, last, in room the queue keeps for it from its creation, so it needs no memory
 * and cannot fail. At most one waits: an announcement not yet drained is taken out first.
 */
void weivel_events_announce_deadline(weivel_events *events, const weivel_deadline_event *deadline);

#endif
