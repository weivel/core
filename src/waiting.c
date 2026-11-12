#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constraints.h"
#include "core_internal.h"
#include "registry.h"
#include "sized.h"

void weivel_waiting_free(weivel_want *want) {
    weivel_constraints_free(want->constrained);
    free(want);
}

/** Takes a want off every list and its deadline, and ends its handle; settling and withdrawing both start here. */
static void retire(weivel_core *core, weivel_want *want) {
    weivel_list_remove(&want->by_slot);
    weivel_list_remove(&want->by_consumer);
    if (want->timed || want->constrained != NULL) {
        weivel_deadlines_cancel(core->deadlines, want->handle);
    }
    weivel_constraints_free(want->constrained);
    want->constrained = NULL;
    weivel_handles_release(core->handles, want->handle, WEIVEL_RECORD_WANT);
    if (want->slot != NULL) {
        weivel_registry_release_slot(core->registry, want->slot);
    }
}

weivel_status weivel_waiting_settle(weivel_core *core, weivel_want *want, weivel_registration *registration,
                                    bool degraded) {
    weivel_status status = weivel_registry_hold(core, registration, want->consumer);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    retire(core, want);
    weivel_want_event settled = {want->handle, want->consumer->handle, registration->handle, {0, NULL, NULL},
                                 degraded ? 1u : 0u};
    weivel_events_push_want(core->events, &settled, NULL, NULL);
    free(want);
    return WEIVEL_STATUS_OK;
}

void weivel_waiting_reject(weivel_core *core, weivel_want *want, uint32_t code, const char *detail, const char *fix) {
    retire(core, want);
    weivel_want_event rejected = {want->handle, want->consumer->handle, 0, {code, NULL, NULL}, 0};
    weivel_events_push_want(core->events, &rejected, detail, fix);
    free(want);
}

void weivel_waiting_withdraw(weivel_core *core, weivel_want *want) {
    retire(core, want);
    free(want);
}

void weivel_waiting_arrival(weivel_core *core, weivel_slot *slot) {
    weivel_registration *registration = slot->registration;
    weivel_link *head = &slot->waiters;
    weivel_link *link = head->next;
    while (link != head) {
        weivel_link *next = link->next;
        weivel_want *want = WEIVEL_RECORD_OF(link, weivel_want, by_slot);
        if (weivel_registry_accepts(want->provider, registration)) {
            if (weivel_waiting_settle(core, want, registration, false) != WEIVEL_STATUS_OK) {
                weivel_core_log(core, WEIVEL_LOG_ERROR, "memory ran out settling a want, so it still waits");
            }
        } else {
            weivel_reach mismatch;
            weivel_reach_refuse_provider(core, want->consumer, slot->id, want->provider,
                                         weivel_registry_owner_name(registration), &mismatch);
        }
        link = next;
    }
}

static void time_out(weivel_core *core, weivel_want *want) {
    char detail[384];
    char fix[384];
    snprintf(detail, sizeof detail, "waited %" PRId64 "ms for service \"%s\" and nothing registered it",
             want->timeout_millis, want->slot->id);
    snprintf(fix, sizeof fix, "install a plugin that provides \"%s\", or get it and carry on without it",
             want->slot->id);
    weivel_waiting_reject(core, want, WEIVEL_REFUSAL_TIMED_OUT, detail, fix);
}

void weivel_waiting_due(weivel_core *core, weivel_handle subject, int64_t now) {
    void *record;
    if (weivel_handles_resolve(core->handles, subject, WEIVEL_RECORD_WANT, &record) != WEIVEL_STATUS_OK) {
        weivel_deadlines_cancel(core->deadlines, subject);
        return;
    }
    weivel_want *want = record;
    if (want->constrained != NULL) {
        weivel_constraints_due(core, want, now);
    } else {
        time_out(core, want);
    }
}

static weivel_want *new_want(weivel_core *core, weivel_plugin *plugin, weivel_slot *slot) {
    weivel_want *want = calloc(1, sizeof *want);
    if (want == NULL) {
        return NULL;
    }
    if (weivel_handles_allocate(core->handles, WEIVEL_RECORD_WANT, want, &want->handle) != WEIVEL_STATUS_OK) {
        free(want);
        return NULL;
    }
    want->slot = slot;
    want->consumer = plugin;
    weivel_list_init(&want->by_slot);
    weivel_list_init(&want->by_consumer);
    return want;
}

static int64_t deadline_of(const weivel_want_options *options) {
    return options->now > INT64_MAX - options->timeout_millis ? INT64_MAX : options->now + options->timeout_millis;
}

static weivel_status enqueue(weivel_core *core, weivel_want *want, const weivel_want_options *options) {
    if (options->timed == WEIVEL_CHOICE_YES) {
        weivel_status status = weivel_deadlines_add(core->deadlines, deadline_of(options), want->handle);
        if (status != WEIVEL_STATUS_OK) {
            return status;
        }
        want->timed = true;
        want->timeout_millis = options->timeout_millis;
    }
    weivel_list_append(&want->slot->waiters, &want->by_slot);
    weivel_list_append(&want->consumer->wants, &want->by_consumer);
    return WEIVEL_STATUS_OK;
}

static weivel_status refuse_unpolled(weivel_core *core, const char *id) {
    char detail[512];
    snprintf(detail, sizeof detail,
             "weivel_want_service asked for service \"%s\", which the manifest constrains, without a poll interval",
             id);
    return weivel_core_refuse_malformed(
        core, detail,
        "give weivel_want_options a poll_millis of 1 or more; a budget is optional, and without one the want polls "
        "until a provider meets its constraints");
}

static weivel_status decide(weivel_core *core, weivel_want *want, const char *id, const weivel_reach *reach,
                            const weivel_want_options *options) {
    if (reach->code != WEIVEL_REFUSAL_NONE) {
        weivel_waiting_reject(core, want, reach->code, reach->detail, reach->fix);
        return WEIVEL_STATUS_OK;
    }
    if (reach->late) {
        weivel_reach_log_late(core, want->consumer, "want", id);
        weivel_reach stopped;
        weivel_reach_stopped(id, &stopped);
        weivel_waiting_reject(core, want, WEIVEL_REFUSAL_STOPPED, stopped.detail, stopped.fix);
        return WEIVEL_STATUS_OK;
    }
    if (weivel_constraints_apply(reach->wanted)) {
        if (options->poll_millis <= 0) {
            return refuse_unpolled(core, id);
        }
        weivel_list_append(&want->slot->pollers, &want->by_slot);
        weivel_list_append(&want->consumer->wants, &want->by_consumer);
        return weivel_constraints_start(core, want, reach->wanted, options);
    }
    if (want->slot->registration != NULL) {
        return weivel_waiting_settle(core, want, want->slot->registration, false);
    }
    return enqueue(core, want, options);
}

static weivel_status want_service(weivel_core *core, weivel_handle consumer, const char *id,
                                  const weivel_want_options *options, weivel_handle *handle) {
    weivel_plugin *plugin;
    weivel_status status = weivel_core_opened_plugin(core, consumer, &plugin);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    weivel_reach reach;
    weivel_reach_check(core, plugin, id, &reach);
    weivel_slot *slot = NULL;
    if (reach.code == WEIVEL_REFUSAL_NONE && (slot = weivel_registry_slot(core->registry, id)) == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_want *want = new_want(core, plugin, slot);
    if (want == NULL) {
        if (slot != NULL) {
            weivel_registry_release_slot(core->registry, slot);
        }
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    want->provider = reach.provider;
    weivel_handle answered = want->handle;
    status = decide(core, want, id, &reach, options);
    if (status != WEIVEL_STATUS_OK) {
        weivel_waiting_withdraw(core, want);
        return status;
    }
    *handle = answered;
    return WEIVEL_STATUS_OK;
}

static bool read_want_options(const weivel_want_options *options, weivel_want_options *resolved) {
    memset(resolved, 0, sizeof *resolved);
    if (options == NULL) {
        return true;
    }
    if (weivel_sized_size(options) < WEIVEL_WANT_OPTIONS_FIRST_SIZE) {
        return false;
    }
    weivel_sized_read(resolved, sizeof *resolved, options);
    bool timed = resolved->timed == WEIVEL_CHOICE_YES;
    bool budgeted = resolved->budgeted == WEIVEL_CHOICE_YES;
    bool timeout_valid = !timed || resolved->timeout_millis >= 0;
    bool budget_valid = (!budgeted || resolved->budget_millis >= 0) && resolved->poll_millis >= 0;
    return resolved->timed <= WEIVEL_CHOICE_YES && resolved->budgeted <= WEIVEL_CHOICE_YES && timeout_valid &&
           budget_valid;
}

weivel_status weivel_want_service(weivel_core *core, weivel_handle plugin, const char *id,
                                  const weivel_want_options *options, weivel_handle *want,
                                  uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_want_options resolved;
    weivel_status status;
    if (want != NULL) {
        *want = 0;
    }
    if (want == NULL || id == NULL || id[0] == '\0') {
        status = weivel_core_refuse_malformed(
            core, "weivel_want_service was given no service id, or no place to write the want",
            "pass a non-empty service id and a pointer to a weivel_handle");
    } else if (!read_want_options(options, &resolved)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_want_service was given options too small, a timed or budgeted that is no weivel_choice, or a "
                  "negative timeout, budget or poll interval",
            "pass weivel_want_options initialised with WEIVEL_INIT, timed and budgeted WEIVEL_CHOICE_YES or NO, and a "
            "timeout, a budget and a poll interval of 0 or more");
    } else {
        status = want_service(core, plugin, id, &resolved, want);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}
