#ifndef WEIVEL_HANDLES_H
#define WEIVEL_HANDLES_H

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

/** Ends a handle; WEIVEL_STATUS_REFUSED when it does not resolve as `kind`. */
weivel_status weivel_handles_release(weivel_handles *handles, weivel_handle handle, uint32_t kind);

/**
 * The generation a slot moves to when released, or 0 when the slot must be retired.
 * @implNote Wrapping back to 1 would let a handle four billion releases old resolve again, so a slot that reaches
 * the last generation is never reused.
 */
uint32_t weivel_handles_next_generation(uint32_t generation);

#endif
