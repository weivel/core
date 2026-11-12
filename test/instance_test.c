#include "check.h"
#include "core_internal.h"

static weivel_core *create_core(uint32_t minimum_log_level) {
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.minimum_log_level = minimum_log_level;
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    return core;
}

static void drain_and_release(weivel_core *core) {
    weivel_event event = WEIVEL_INIT(weivel_event);
    while (weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK) {
        weivel_event_release(&event);
    }
}

static void a_core_is_created_and_destroyed(void) {
    weivel_core *core = NULL;
    uint32_t pending = 99;
    CHECK(weivel_core_create(NULL, &core, &pending) == WEIVEL_STATUS_OK);
    CHECK(core != NULL);
    CHECK(pending == 0);
    CHECK(weivel_core_destroy(core) == WEIVEL_STATUS_OK);
}

static void malformed_calls_are_answered_not_crashed(void) {
    weivel_core *core = NULL;
    weivel_event event = WEIVEL_INIT(weivel_event);
    int64_t deadline = 0;
    weivel_core_options too_small = WEIVEL_INIT(weivel_core_options);
    too_small.size = 4;

    CHECK(weivel_core_create(NULL, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_core_create(&too_small, &core, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(core == NULL);
    CHECK(weivel_core_destroy(NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_next_event(NULL, &event, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_event_release(NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_next_deadline(NULL, &deadline, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_advance(NULL, 0, NULL) == WEIVEL_STATUS_MALFORMED);

    weivel_event small_event = WEIVEL_INIT(weivel_event);
    small_event.size = 8;
    CHECK(weivel_event_release(&small_event) == WEIVEL_STATUS_MALFORMED);
    weivel_core_destroy(core);
}

static void a_malformed_call_on_a_core_queues_its_refusal(void) {
    weivel_core *core = create_core(WEIVEL_LOG_DEFAULT);
    uint32_t pending = 0;
    weivel_event event = WEIVEL_INIT(weivel_event);

    CHECK(weivel_next_deadline(core, NULL, &pending) == WEIVEL_STATUS_MALFORMED);
    CHECK(pending == 1);
    CHECK(weivel_next_event(core, NULL, &pending) == WEIVEL_STATUS_MALFORMED);
    CHECK(pending == 2);

    CHECK(weivel_next_event(core, &event, &pending) == WEIVEL_STATUS_OK);
    CHECK(event.kind == WEIVEL_EVENT_REFUSAL);
    CHECK(event.data.refusal.code == WEIVEL_REFUSAL_MALFORMED_CALL);
    CHECK(strstr(event.data.refusal.detail, "weivel_next_deadline") != NULL);
    CHECK(strlen(event.data.refusal.fix) > 0);
    weivel_event_release(&event);
    drain_and_release(core);
    weivel_core_destroy(core);
}

static void events_drain_once_in_order_and_their_strings_outlive_later_calls(void) {
    weivel_core *core = create_core(WEIVEL_LOG_DEFAULT);
    weivel_event first = WEIVEL_INIT(weivel_event);
    weivel_event second = WEIVEL_INIT(weivel_event);
    weivel_event third = WEIVEL_INIT(weivel_event);
    uint32_t pending = 0;

    weivel_next_deadline(core, NULL, NULL);
    weivel_next_event(core, NULL, NULL);
    for (int index = 0; index < 40; index++) {
        weivel_next_deadline(core, NULL, NULL);
    }

    CHECK(weivel_next_event(core, &first, &pending) == WEIVEL_STATUS_OK);
    CHECK(pending == 41);
    CHECK(weivel_next_event(core, &second, &pending) == WEIVEL_STATUS_OK);
    CHECK(pending == 40);
    drain_and_release(core);
    CHECK(weivel_next_event(core, &third, &pending) == WEIVEL_STATUS_NONE);
    CHECK(pending == 0);
    CHECK(third.kind == WEIVEL_EVENT_NONE);
    CHECK(third.storage == NULL);

    CHECK(strstr(first.data.refusal.detail, "weivel_next_deadline") != NULL);
    CHECK(strstr(second.data.refusal.detail, "weivel_next_event") != NULL);
    weivel_core_destroy(core);
    CHECK(strstr(first.data.refusal.detail, "weivel_next_deadline") != NULL);

    CHECK(weivel_event_release(&second) == WEIVEL_STATUS_OK);
    CHECK(second.kind == WEIVEL_EVENT_NONE);
    CHECK(second.storage == NULL);
    CHECK(strstr(first.data.refusal.fix, "int64_t") != NULL);
    CHECK(weivel_event_release(&first) == WEIVEL_STATUS_OK);
    CHECK(weivel_event_release(&first) == WEIVEL_STATUS_OK);
}

static void with_nothing_pending_there_is_no_deadline(void) {
    weivel_core *core = create_core(WEIVEL_LOG_DEFAULT);
    int64_t deadline = 12345;
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_NONE);
    CHECK(deadline == 12345);
    weivel_core_destroy(core);
}

static void advancing_before_a_deadline_settles_nothing(void) {
    weivel_core *core = create_core(WEIVEL_LOG_DEBUG);
    weivel_event event = WEIVEL_INIT(weivel_event);
    int64_t deadline = 0;
    uint32_t pending = 0;
    drain_and_release(core);

    CHECK(weivel_deadlines_add(core->deadlines, 100, 7) == WEIVEL_STATUS_OK);
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_OK);
    CHECK(deadline == 100);

    CHECK(weivel_advance(core, 99, &pending) == WEIVEL_STATUS_OK);
    CHECK(pending == 0);
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_OK);
    CHECK(deadline == 100);

    CHECK(weivel_advance(core, 100, &pending) == WEIVEL_STATUS_OK);
    CHECK(pending == 1);
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_NONE);
    CHECK(weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK);
    CHECK(event.kind == WEIVEL_EVENT_LOG);
    check_text(event.data.log.text, "1 deadlines came due at 100", "settled log");
    weivel_event_release(&event);
    weivel_core_destroy(core);
}

static void deadlines_come_due_earliest_first_and_can_be_cancelled(void) {
    weivel_deadlines *deadlines = weivel_deadlines_create();
    weivel_handle subject = 0;
    int64_t at = 0;

    weivel_deadlines_add(deadlines, 300, 3);
    weivel_deadlines_add(deadlines, 100, 1);
    weivel_deadlines_add(deadlines, 200, 2);
    weivel_deadlines_add(deadlines, 100, 4);
    CHECK(weivel_deadlines_cancel(deadlines, 2));
    CHECK(!weivel_deadlines_cancel(deadlines, 2));
    CHECK(weivel_deadlines_earliest(deadlines, &at) && at == 100);

    CHECK(weivel_deadlines_take_due(deadlines, 250, &subject) && subject == 1);
    CHECK(weivel_deadlines_take_due(deadlines, 250, &subject) && subject == 4);
    CHECK(!weivel_deadlines_take_due(deadlines, 250, &subject));
    CHECK(weivel_deadlines_take_due(deadlines, 300, &subject) && subject == 3);
    CHECK(!weivel_deadlines_earliest(deadlines, &at));
    weivel_deadlines_destroy(deadlines);
}

int main(void) {
    a_core_is_created_and_destroyed();
    malformed_calls_are_answered_not_crashed();
    a_malformed_call_on_a_core_queues_its_refusal();
    events_drain_once_in_order_and_their_strings_outlive_later_calls();
    with_nothing_pending_there_is_no_deadline();
    advancing_before_a_deadline_settles_nothing();
    deadlines_come_due_earliest_first_and_can_be_cancelled();
    return check_summary("instance_test");
}
