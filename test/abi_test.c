#include <stddef.h>
#include <weivel/core.h>

#include "check.h"

typedef struct newer_options {
    weivel_core_options options;
    uint32_t appended_later;
} newer_options;

typedef struct guarded_event {
    weivel_event event;
    unsigned char guard[32];
} guarded_event;

typedef struct newer_event {
    weivel_event event;
    uint64_t appended_later[4];
} newer_event;

static uint32_t pending_after_create(const weivel_core_options *options) {
    weivel_core *core = NULL;
    uint32_t pending = 99;
    CHECK(weivel_core_create(options, &core, &pending) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
    return pending;
}

static void options_of_the_previous_size_take_defaults_for_what_they_lack(void) {
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.minimum_log_level = WEIVEL_LOG_DEBUG;
    CHECK(pending_after_create(&options) == 1);

    options.size = (uint32_t)offsetof(weivel_core_options, minimum_log_level);
    CHECK(pending_after_create(&options) == 0);
}

static void options_from_a_newer_caller_are_read_up_to_what_the_core_knows(void) {
    newer_options newer = {.options = WEIVEL_INIT(weivel_core_options), .appended_later = 0xffffffffu};
    newer.options.size = (uint32_t)sizeof newer;
    newer.options.minimum_log_level = WEIVEL_LOG_DEBUG;
    CHECK(pending_after_create(&newer.options) == 1);
}

static weivel_core *core_with_one_event(void) {
    weivel_core *core = NULL;
    CHECK(weivel_core_create(NULL, &core, NULL) == WEIVEL_STATUS_OK);
    weivel_next_deadline(core, NULL, NULL);
    return core;
}

static void an_event_is_written_only_within_the_callers_size(void) {
    weivel_core *core = core_with_one_event();
    guarded_event guarded;
    memset(&guarded, 0xab, sizeof guarded);
    guarded.event.size = (uint32_t)sizeof guarded.event;

    CHECK(weivel_next_event(core, &guarded.event, NULL) == WEIVEL_STATUS_OK);
    CHECK(guarded.event.kind == WEIVEL_EVENT_REFUSAL);
    CHECK(weivel_event_release(&guarded.event) == WEIVEL_STATUS_OK);
    for (size_t index = 0; index < sizeof guarded.guard; index++) {
        CHECK(guarded.guard[index] == 0xab);
    }
    weivel_core_destroy(core);
}

static void a_newer_callers_appended_event_fields_read_as_zero(void) {
    weivel_core *core = core_with_one_event();
    newer_event newer;
    memset(&newer, 0xff, sizeof newer);
    newer.event.size = (uint32_t)sizeof newer;

    CHECK(weivel_next_event(core, &newer.event, NULL) == WEIVEL_STATUS_OK);
    for (size_t index = 0; index < 4; index++) {
        CHECK(newer.appended_later[index] == 0);
    }
    CHECK(weivel_event_release(&newer.event) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

static void an_event_smaller_than_its_first_shape_is_malformed(void) {
    weivel_core *core = core_with_one_event();
    weivel_event event = WEIVEL_INIT(weivel_event);
    uint32_t pending = 0;
    event.size = (uint32_t)offsetof(weivel_event, data);
    CHECK(weivel_next_event(core, &event, &pending) == WEIVEL_STATUS_MALFORMED);
    CHECK(pending == 2);
    weivel_core_destroy(core);
}

int main(void) {
    options_of_the_previous_size_take_defaults_for_what_they_lack();
    options_from_a_newer_caller_are_read_up_to_what_the_core_knows();
    an_event_is_written_only_within_the_callers_size();
    a_newer_callers_appended_event_fields_read_as_zero();
    an_event_smaller_than_its_first_shape_is_malformed();
    return check_summary("abi_test");
}
