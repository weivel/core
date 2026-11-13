#ifndef WEIVEL_HANDLES_H
#define WEIVEL_HANDLES_H

#include <stdbool.h>
#include <weivel/core.h>

/**
 * One core's table from handles to the core's own records. A handle is its slot in the low 32 bits and the slot's
 * generation in the high 32; a released slot's generation moves on, so the old handle stops resolving before the
 * slot is reused. Generations start at 1, which is why no handle is 0. Every handle carries the kind of record it
 * was issued for, and resolving it as another kind is refused like a stale one.
 */
typedef struct weivel_handles weivel_handles;

weivel_handles *weivel_handles_create(void);
void weivel_handles_destroy(weivel_handles *handles);

/** Issues a handle for `record`, which the table borrows and never frees. */
weivel_status weivel_handles_allocate(weivel_handles *handles, uint32_t kind, void *record, weivel_handle *handle);

/** WEIVEL_STATUS_REFUSED, with `*record` NULL, when the handle is 0, stale, never issued, or of another kind. */
weivel_status weivel_handles_resolve(const weivel_handles *handles, weivel_handle handle, uint32_t kind,
                                     void **record);

/** How many handles are live, and how many slots the table has ever made, for a test of churn. */
uint32_t weivel_handles_live(const weivel_handles *handles);
uint32_t weivel_handles_slots(const weivel_handles *handles);

/** Ends a handle; WEIVEL_STATUS_REFUSED when it does not resolve as `kind`. */
weivel_status weivel_handles_release(weivel_handles *handles, weivel_handle handle, uint32_t kind);

/**
 * Ends a handle as weivel_handles_release does, and has its slot remember `ending`, how what it named ended, until
 * the slot's next handle ends in turn.
 */
weivel_status weivel_handles_release_ending(weivel_handles *handles, weivel_handle handle, uint32_t kind,
                                            uint32_t ending);

/**
 * The ending a released handle's slot remembers for it, or 0 when the handle never ended, its slot has ended
 * another since, or it ended with no ending given.
 */
uint32_t weivel_handles_ending(const weivel_handles *handles, weivel_handle handle);

/** Whether the handle is live and of `kind`. */
bool weivel_handles_live_as(const weivel_handles *handles, weivel_handle handle, uint32_t kind);

/**
 * Takes back a handle issued by the call now running and never handed out, as though it had never been issued: the
 * slot keeps its generation and returns to the head of the free list, or, when the issue made it, is unmade. A call
 * undoing a change takes its handles back newest first, so the table ends exactly as it began.
 */
void weivel_handles_unissue(weivel_handles *handles, weivel_handle handle, uint32_t kind);

/**
 * Moves a live handle's slot to `generation` and answers the handle that now names it, or 0 when `handle` is not
 * live. @implNote Exists so a test can reach a slot's last generation without four billion releases.
 */
weivel_handle weivel_handles_set_generation(weivel_handles *handles, weivel_handle handle, uint32_t kind,
                                            uint32_t generation);

/**
 * The generation a slot moves to when released, or 0 when the slot must be retired.
 * @implNote Wrapping back to 1 would let a handle four billion releases old resolve again, so a slot that reaches
 * the last generation is never reused.
 */
uint32_t weivel_handles_next_generation(uint32_t generation);

#endif
