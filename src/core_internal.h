#ifndef WEIVEL_CORE_INTERNAL_H
#define WEIVEL_CORE_INTERNAL_H

#include <stddef.h>
#include <weivel/core.h>

#include "compatibility.h"
#include "deadlines.h"
#include "events.h"
#include "handles.h"
#include "mutex.h"
#include "plugins.h"

/** Every field but `mutex` is touched only while `mutex` is held. */
struct weivel_core {
    weivel_mutex *mutex;
    weivel_events *events;
    weivel_handles *handles;
    weivel_deadlines *deadlines;
    weivel_plugins *plugins;
    weivel_core_options options;
    /** The applications and api the options gave, the applications copied into `host_storage`. */
    weivel_host host;
    void *host_storage;
};

/** The kind of record each handle names. */
typedef enum weivel_record_kind {
    WEIVEL_RECORD_PLUGIN = 1
} weivel_record_kind;

/** The smallest `size` each public struct accepts: its shape when first published, fixed forever. */
#define WEIVEL_CORE_OPTIONS_FIRST_SIZE (offsetof(weivel_core_options, initial_event_capacity) + sizeof(uint32_t))
#define WEIVEL_EVENT_FIRST_SIZE (offsetof(weivel_event, data) + WEIVEL_EVENT_PAYLOAD_WORDS * sizeof(uint64_t))
#define WEIVEL_SUPPORT_FIRST_SIZE (offsetof(weivel_support, storage) + sizeof(void *))

/** Queues a log event when `level` reaches the core's minimum. Called with the core's mutex held. */
void weivel_core_log(weivel_core *core, uint32_t level, const char *text);

/** Queues a malformed-call refusal and answers WEIVEL_STATUS_MALFORMED. Called with the core's mutex held. */
weivel_status weivel_core_refuse_malformed(weivel_core *core, const char *detail, const char *fix);

/** Writes how many events wait into `pending_events` when it is given. Called with the core's mutex held. */
void weivel_core_report_pending(const weivel_core *core, uint32_t *pending_events);

#endif
