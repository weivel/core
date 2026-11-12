#include "core_internal.h"
#include "memory.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "manifest.h"
#include "sized.h"

weivel_status weivel_core_refuse_malformed(weivel_core *core, const char *detail, const char *fix) {
    weivel_status status = weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_MALFORMED_CALL, detail, fix);
    return status == WEIVEL_STATUS_OK ? WEIVEL_STATUS_MALFORMED : status;
}

static void report_pending(const weivel_core *core, uint32_t *pending_events) {
    if (pending_events != NULL) {
        *pending_events = weivel_events_pending(core->events);
    }
}

void weivel_core_end_call(weivel_core *core, uint32_t *pending_events) {
    weivel_events_unreserve(core->events, core->notices);
    core->notices = 0;
    weivel_registry_finish(core);
    weivel_capabilities_finish(core);
    report_pending(core, pending_events);
}

weivel_status weivel_core_reserve_notices(weivel_core *core, uint32_t count) {
    weivel_status status = weivel_events_reserve(core->events, count);
    if (status == WEIVEL_STATUS_OK) {
        core->notices += count;
    }
    return status;
}

void weivel_core_notify_watch(weivel_core *core, const weivel_watch_event *heard) {
    core->notices--;
    weivel_events_deliver_watch(core->events, heard);
}

void weivel_core_notify_capability(weivel_core *core, const weivel_capability_event *heard) {
    core->notices--;
    weivel_events_deliver_capability(core->events, heard);
}

void weivel_core_log(weivel_core *core, uint32_t level, const char *text) {
    if (level >= core->options.minimum_log_level &&
        weivel_events_push_log(core->events, level, text) != WEIVEL_STATUS_OK) {
        core->dropped_logs++;
    }
}

static weivel_core_options resolve_options(const weivel_core_options *options) {
    weivel_core_options resolved;
    memset(&resolved, 0, sizeof resolved);
    if (options != NULL) {
        weivel_sized_read(&resolved, sizeof resolved, options);
    }
    resolved.size = (uint32_t)sizeof resolved;
    if (resolved.minimum_log_level == WEIVEL_LOG_DEFAULT) {
        resolved.minimum_log_level = WEIVEL_LOG_WARNING;
    }
    if (resolved.api == 0) {
        resolved.api = WEIVEL_API_VERSION;
    }
    return resolved;
}

static void free_core(weivel_core *core) {
    weivel_capabilities_destroy(core->capabilities);
    weivel_registry_destroy(core->registry);
    weivel_vocabulary_free(&core->vocabulary);
    weivel_plugins_destroy(core->plugins);
    free(core->well_known_storage);
    free(core->host_storage);
    weivel_deadlines_destroy(core->deadlines);
    weivel_handles_destroy(core->handles);
    weivel_events_destroy(core->events);
    weivel_mutex_destroy(core->mutex);
    free(core);
}

static weivel_core *allocate_core(const weivel_core_options *options) {
    weivel_core *core = weivel_memory_allocate_zeroed(1, sizeof *core);
    if (core == NULL) {
        return NULL;
    }
    core->options = *options;
    weivel_plugins_init_host(&core->host_plugin);
    core->mutex = weivel_mutex_create();
    core->events = weivel_events_create(options->initial_event_capacity);
    core->handles = weivel_handles_create();
    core->deadlines = weivel_deadlines_create();
    core->plugins = weivel_plugins_create();
    core->registry = weivel_registry_create();
    core->capabilities = weivel_capabilities_create();
    core->host.api = options->api;
    bool host_copied = weivel_strings_copy(&options->applications, &core->host.applications, &core->host_storage);
    core->options.applications = core->host.applications;
    bool well_known_copied =
        weivel_strings_copy(&options->well_known_services, &core->well_known, &core->well_known_storage);
    core->options.well_known_services = core->well_known;
    bool vocabulary_copied = weivel_vocabulary_copy(&options->constraint_vocabulary, &core->vocabulary);
    core->options.constraint_vocabulary.items = core->vocabulary.entries;
    core->options.constraint_vocabulary.count = core->vocabulary.count;
    if (!host_copied || !well_known_copied || !vocabulary_copied || core->mutex == NULL || core->events == NULL ||
        core->handles == NULL || core->deadlines == NULL || core->plugins == NULL || core->registry == NULL ||
        core->capabilities == NULL) {
        free_core(core);
        return NULL;
    }
    return core;
}

weivel_status weivel_core_create(const weivel_core_options *options, weivel_core **core, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    *core = NULL;
    if (options != NULL && weivel_sized_size(options) < WEIVEL_CORE_OPTIONS_FIRST_SIZE) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_core_options resolved = resolve_options(options);
    if (resolved.initial_event_capacity > WEIVEL_CORE_MAXIMUM_INITIAL_EVENT_CAPACITY ||
        !weivel_strings_readable(&resolved.applications) || !weivel_strings_readable(&resolved.well_known_services) ||
        !weivel_vocabulary_readable(&resolved.constraint_vocabulary)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_core *created = allocate_core(&resolved);
    if (created == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_core_log(created, WEIVEL_LOG_DEBUG, "core created");
    report_pending(created, pending_events);
    *core = created;
    return WEIVEL_STATUS_OK;
}

weivel_status weivel_core_destroy(weivel_core *core) {
    if (core != NULL) {
        free_core(core);
    }
    return WEIVEL_STATUS_OK;
}

static weivel_event no_event(void) {
    weivel_event event;
    memset(&event, 0, sizeof event);
    event.size = (uint32_t)sizeof event;
    return event;
}

static bool is_sized_event(const weivel_event *event) {
    return event != NULL && event->size >= WEIVEL_EVENT_FIRST_SIZE;
}

weivel_status weivel_next_event(weivel_core *core, weivel_event *event, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (!is_sized_event(event)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_next_event was given no event struct, or one whose size is too small",
            "pass a weivel_event initialised with WEIVEL_INIT(weivel_event)");
    } else if (event->kind != WEIVEL_EVENT_NONE) {
        status = weivel_core_refuse_malformed(core, "weivel_next_event was given an event still holding one",
                                              "pass the held event to weivel_event_release first");
    } else {
        weivel_event taken = no_event();
        status = weivel_events_pop(core->events, &taken) ? WEIVEL_STATUS_OK : WEIVEL_STATUS_NONE;
        weivel_sized_write(event, &taken, sizeof taken);
    }
    weivel_core_end_call(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

static bool is_zero_sized_event(const weivel_event *event) {
    return event != NULL && event->size == 0;
}

weivel_status weivel_event_release(weivel_event *event) {
    if (is_zero_sized_event(event)) {
        return WEIVEL_STATUS_OK;
    }
    if (!is_sized_event(event)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    free(event->storage);
    weivel_event empty = no_event();
    weivel_sized_write(event, &empty, sizeof empty);
    return WEIVEL_STATUS_OK;
}

#ifdef WEIVEL_TESTING

void weivel_core_census(weivel_core *core, weivel_census *census) {
    weivel_mutex_lock(core->mutex);
    census->plugins = weivel_plugins_count(core->plugins);
    census->slots = core->registry->slot_count;
    census->holdings = core->registry->holding_count;
    census->live_handles = weivel_handles_live(core->handles);
    census->handle_slots = weivel_handles_slots(core->handles);
    census->deadlines = (uint32_t)weivel_deadlines_count(core->deadlines);
    census->capabilities = core->capabilities->count;
    census->provisions = core->capabilities->provision_count;
    census->capability_watches = core->capabilities->watch_count;
    census->capability_holdings = core->capabilities->holding_count;
    census->wants = core->registry->want_count;
    census->watches = core->registry->watch_count;
    census->reserved_events = weivel_events_reserved(core->events);
    weivel_mutex_unlock(core->mutex);
}

#endif

weivel_status weivel_next_deadline(weivel_core *core, int64_t *deadline, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (deadline == NULL) {
        status = weivel_core_refuse_malformed(core, "weivel_next_deadline was given no place to write the deadline",
                                              "pass a pointer to an int64_t");
    } else {
        status = weivel_deadlines_earliest(core->deadlines, deadline) ? WEIVEL_STATUS_OK : WEIVEL_STATUS_NONE;
    }
    weivel_core_end_call(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

static void log_settled(weivel_core *core, uint32_t settled, int64_t now) {
    char text[96];
    snprintf(text, sizeof text, "%" PRIu32 " deadlines came due at %" PRId64, settled, now);
    weivel_core_log(core, WEIVEL_LOG_DEBUG, text);
}

weivel_status weivel_advance(weivel_core *core, int64_t now, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    uint32_t settled = 0;
    weivel_handle subject;
    while (weivel_deadlines_next_due(core->deadlines, now, &subject)) {
        weivel_waiting_due(core, subject, now);
        settled++;
    }
    if (settled > 0) {
        log_settled(core, settled, now);
    }
    weivel_core_end_call(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return WEIVEL_STATUS_OK;
}
