#ifndef WEIVEL_CORE_INTERNAL_H
#define WEIVEL_CORE_INTERNAL_H

#include <stddef.h>
#include <weivel/core.h>

#include "deadlines.h"
#include "events.h"
#include "handles.h"
#include "mutex.h"

/** Every field but `mutex` is touched only while `mutex` is held. */
struct weivel_core {
    weivel_mutex *mutex;
    weivel_events *events;
    weivel_handles *handles;
    weivel_deadlines *deadlines;
    weivel_core_options options;
};

/** The smallest `size` each public struct accepts: its shape when first published, fixed forever. */
#define WEIVEL_CORE_OPTIONS_FIRST_SIZE (offsetof(weivel_core_options, initial_event_capacity) + sizeof(uint32_t))
#define WEIVEL_EVENT_FIRST_SIZE (offsetof(weivel_event, data) + WEIVEL_EVENT_PAYLOAD_WORDS * sizeof(uint64_t))

/** Queues a log event when `level` reaches the core's minimum. Called with the core's mutex held. */
void weivel_core_log(weivel_core *core, uint32_t level, const char *text);

#endif
