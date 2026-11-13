#include "core_internal.h"

static bool live_as(const weivel_core *core, weivel_handle handle, uint32_t kind) {
    return weivel_handles_live_as(core->handles, handle, kind);
}

/** The condition of a live want or watch, else how one ended, else 0 for a handle that names neither. */
static uint32_t condition_of(const weivel_core *core, weivel_handle handle) {
    if (live_as(core, handle, WEIVEL_RECORD_WANT)) {
        return WEIVEL_HANDLE_WAITING;
    }
    if (live_as(core, handle, WEIVEL_RECORD_WATCH) || live_as(core, handle, WEIVEL_RECORD_CAPABILITY_WATCH)) {
        return WEIVEL_HANDLE_WATCHING;
    }
    return weivel_handles_ending(core->handles, handle);
}

weivel_status weivel_handle_state(weivel_core *core, weivel_handle handle, uint32_t *condition,
                                  uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (condition == NULL) {
        status = weivel_core_refuse_malformed(core, "weivel_handle_state was given no place to write the condition",
                                              "pass a pointer to a uint32_t");
    } else {
        *condition = condition_of(core, handle);
        status = *condition == 0 ? WEIVEL_STATUS_REFUSED : WEIVEL_STATUS_OK;
    }
    weivel_core_end_call(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}
