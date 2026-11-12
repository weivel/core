#ifndef WEIVEL_DEADLINES_H
#define WEIVEL_DEADLINES_H

#include <stdbool.h>
#include <weivel/core.h>

/**
 * The times a core waits on, each naming the handle of whatever settles when it passes. Earliest first; two
 * deadlines at the same time come due in the order they were added.
 */
typedef struct weivel_deadlines weivel_deadlines;

weivel_deadlines *weivel_deadlines_create(void);
void weivel_deadlines_destroy(weivel_deadlines *deadlines);

weivel_status weivel_deadlines_add(weivel_deadlines *deadlines, int64_t at, weivel_handle subject);

/** Removes every deadline naming `subject`; false when there was none. */
bool weivel_deadlines_cancel(weivel_deadlines *deadlines, weivel_handle subject);

/** False when nothing waits on time. */
bool weivel_deadlines_earliest(const weivel_deadlines *deadlines, int64_t *at);

/** Removes the earliest deadline if it is at or before `now` and names its subject; false when none is due. */
bool weivel_deadlines_take_due(weivel_deadlines *deadlines, int64_t now, weivel_handle *subject);

#endif
