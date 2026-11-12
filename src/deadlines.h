#ifndef WEIVEL_DEADLINES_H
#define WEIVEL_DEADLINES_H

#include <stdbool.h>
#include <stddef.h>
#include <weivel/core.h>

/**
 * The times a core waits on, each naming the handle of whatever settles when it passes, one deadline per handle.
 * Earliest first; two deadlines at the same time come due in the order they were added. Adding, cancelling and
 * taking the earliest each cost O(log n).
 */
typedef struct weivel_deadlines weivel_deadlines;

weivel_deadlines *weivel_deadlines_create(void);
void weivel_deadlines_destroy(weivel_deadlines *deadlines);

/**
 * Sets the deadline naming `subject`, replacing any it had. A replacement moves the deadline it had, so it needs no
 * memory and always answers WEIVEL_STATUS_OK; only a subject with no deadline yet can run out of memory, and then
 * nothing changes.
 */
weivel_status weivel_deadlines_add(weivel_deadlines *deadlines, int64_t at, weivel_handle subject);

/** Removes the deadline naming `subject`; false when there was none. */
bool weivel_deadlines_cancel(weivel_deadlines *deadlines, weivel_handle subject);

size_t weivel_deadlines_count(const weivel_deadlines *deadlines);

/** False when nothing waits on time. */
bool weivel_deadlines_earliest(const weivel_deadlines *deadlines, int64_t *at);

/** Names the subject of the earliest deadline if it is at or before `now`, leaving it set; false when none is due. */
bool weivel_deadlines_next_due(const weivel_deadlines *deadlines, int64_t now, weivel_handle *subject);

/** Removes the earliest deadline if it is at or before `now` and names its subject; false when none is due. */
bool weivel_deadlines_take_due(weivel_deadlines *deadlines, int64_t now, weivel_handle *subject);

#endif
