#ifndef WEIVEL_CORE_INTERNAL_H
#define WEIVEL_CORE_INTERNAL_H

#include <stddef.h>
#include <weivel/core.h>

#include "capabilities.h"
#include "compatibility.h"
#include "constraints.h"
#include "deadlines.h"
#include "events.h"
#include "handles.h"
#include "mutex.h"
#include "plugins.h"
#include "registry.h"

/** Every field but `mutex` is touched only while `mutex` is held. */
struct weivel_core {
    weivel_mutex *mutex;
    weivel_events *events;
    weivel_handles *handles;
    weivel_deadlines *deadlines;
    weivel_plugins *plugins;
    weivel_registry *registry;
    weivel_capabilities *capabilities;
    weivel_core_options options;
    /** The applications and api the options gave, the applications copied into `host_storage`. */
    weivel_host host;
    void *host_storage;
    /** The ids any plugin may register, copied into `well_known_storage`. */
    weivel_strings well_known;
    void *well_known_storage;
    /** The constraint vocabulary the options gave, copied. */
    weivel_vocabulary vocabulary;
};

/** The kind of record each handle names. */
typedef enum weivel_record_kind {
    WEIVEL_RECORD_PLUGIN = 1,
    WEIVEL_RECORD_REGISTRATION = 2,
    WEIVEL_RECORD_WANT = 3,
    WEIVEL_RECORD_WATCH = 4,
    WEIVEL_RECORD_PROVISION = 5,
    WEIVEL_RECORD_CAPABILITY_WATCH = 6
} weivel_record_kind;

/** The smallest `size` each public struct accepts: its shape when first published, fixed forever. */
#define WEIVEL_CORE_OPTIONS_FIRST_SIZE (offsetof(weivel_core_options, initial_event_capacity) + sizeof(uint32_t))
#define WEIVEL_EVENT_FIRST_SIZE (offsetof(weivel_event, data) + WEIVEL_EVENT_PAYLOAD_WORDS * sizeof(uint64_t))
#define WEIVEL_SUPPORT_FIRST_SIZE (offsetof(weivel_support, storage) + sizeof(void *))
#define WEIVEL_ACTIVATION_PLAN_FIRST_SIZE (offsetof(weivel_activation_plan, storage) + sizeof(void *))
#define WEIVEL_PLUGIN_OPTIONS_FIRST_SIZE (offsetof(weivel_plugin_options, payload) + sizeof(uint64_t))
#define WEIVEL_WANT_OPTIONS_FIRST_SIZE (offsetof(weivel_want_options, timeout_millis) + sizeof(int64_t))
#define WEIVEL_CAPACITY_FIRST_SIZE (offsetof(weivel_capacity, serves) + sizeof(uint32_t))
#define WEIVEL_SERVICE_LIST_FIRST_SIZE (offsetof(weivel_service_list, storage) + sizeof(void *))
#define WEIVEL_CAPABILITY_LIST_FIRST_SIZE (offsetof(weivel_capability_list, storage) + sizeof(void *))

/** Queues a log event when `level` reaches the core's minimum. Called with the core's mutex held. */
void weivel_core_log(weivel_core *core, uint32_t level, const char *text);

/** Queues a malformed-call refusal and answers WEIVEL_STATUS_MALFORMED. Called with the core's mutex held. */
weivel_status weivel_core_refuse_malformed(weivel_core *core, const char *detail, const char *fix);

/** Writes how many events wait into `pending_events` when it is given. Called with the core's mutex held. */
void weivel_core_report_pending(const weivel_core *core, uint32_t *pending_events);

/**
 * Resolves a plugin handle, refusing a stale one with a WEIVEL_REFUSAL_STALE_HANDLE event. Called with the core's
 * mutex held.
 */
weivel_status weivel_core_plugin(weivel_core *core, weivel_handle handle, weivel_plugin **plugin);

/**
 * Resolves a plugin handle for a call into the registry: refused when stale or never opened. Called with the
 * core's mutex held.
 */
weivel_status weivel_core_opened_plugin(weivel_core *core, weivel_handle handle, weivel_plugin **plugin);

/** How many records a core holds now, so a test can show churn leaves nothing behind. */
typedef struct weivel_census {
    uint32_t plugins;
    uint32_t slots;
    uint32_t holdings;
    uint32_t live_handles;
    uint32_t handle_slots;
    uint32_t deadlines;
    uint32_t capabilities;
    uint32_t provisions;
    uint32_t capability_watches;
} weivel_census;

/** Counts a core's records. Takes the core's mutex. */
void weivel_core_census(weivel_core *core, weivel_census *census);

#endif
