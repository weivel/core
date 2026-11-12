#include <stdbool.h>
#include <string.h>
#include <weivel/core.h>

#include "core_internal.h"
#include "registry_support.h"

static bool same_live_counts(const weivel_census *left, const weivel_census *right) {
    return left->plugins == right->plugins && left->slots == right->slots && left->holdings == right->holdings &&
           left->live_handles == right->live_handles && left->deadlines == right->deadlines;
}

static void churn_round(weivel_core *core, weivel_handle stable) {
    plugin_spec spec = {"forge:5", "churn", {"clock", "churn:other"}, {NULL}, {"churn:s"}};
    weivel_handle plugin = open_spec(core, &spec);
    register_service(core, plugin, "churn:s");
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, plugin, "clock", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_get_service(core, stable, "churn:s", &got, NULL) == WEIVEL_STATUS_OK);
    want_untimed(core, plugin, "churn:other");
    want_within(core, plugin, "churn:other", 0, 1000);
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, plugin, "churn:other", &watch, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_get_service(core, plugin, "undeclared:id", &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_get_service(core, plugin, "clock", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_forget_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    forget(core);
}

/**
 * The same name registered, used, released and forgotten thousands of times leaves the core no larger: every live
 * count returns to where it began, and the handle table, having grown once, reuses its slots from then on.
 */
static void churn_leaves_nothing_behind(void) {
    enum { ROUNDS = 3000 };
    weivel_core *core = core_knowing(NULL, 0);
    register_service(core, WEIVEL_HOST, "clock");
    plugin_spec stable_spec = {"stable", NULL, {"churn:s"}, {NULL}, {NULL}};
    weivel_handle stable = open_spec(core, &stable_spec);
    weivel_census before;
    weivel_core_census(core, &before);
    churn_round(core, stable);
    weivel_census after_one;
    weivel_core_census(core, &after_one);
    CHECK(same_live_counts(&before, &after_one));
    uint32_t drifted = 0;
    for (uint32_t round = 1; round < ROUNDS; round++) {
        churn_round(core, stable);
        weivel_census now;
        weivel_core_census(core, &now);
        drifted += !same_live_counts(&before, &now) || now.handle_slots != after_one.handle_slots;
    }
    CHECK(drifted == 0);
    weivel_census last;
    weivel_core_census(core, &last);
    CHECK(last.plugins == 1 && last.slots == 1 && last.holdings == 0 && last.deadlines == 0);
    CHECK(last.live_handles == 2);
    CHECK(last.handle_slots == after_one.handle_slots);
    weivel_core_destroy(core);
}

static void a_slot_goes_when_nothing_uses_it(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"calendar", NULL, {"other:s"}, {NULL}, {"calendar:events"}};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_census census;
    weivel_core_census(core, &census);
    CHECK(census.slots == 0);
    weivel_handle registration = register_service(core, plugin, "calendar:events");
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, plugin, "other:s", &watch, NULL) == WEIVEL_STATUS_OK);
    weivel_handle want = want_within(core, plugin, "other:s", 0, 5);
    weivel_core_census(core, &census);
    CHECK(census.slots == 2);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_unregister(core, watch, NULL) == WEIVEL_STATUS_OK);
    weivel_core_census(core, &census);
    CHECK(census.slots == 1);
    CHECK(weivel_advance(core, 5, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(settlement_of(want) != NULL);
    weivel_core_census(core, &census);
    CHECK(census.slots == 0 && census.deadlines == 0);
    weivel_core_destroy(core);
}

static void fail_next_holding(weivel_core *core) {
    core->registry->fail_next_holding = true;
}

static void a_get_that_cannot_be_held_hands_nothing_over(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"calendar", NULL, {"clock"}, {NULL}, {NULL}};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_handle registration = register_service(core, WEIVEL_HOST, "clock");
    fail_next_holding(core);
    weivel_handle got = 5;
    CHECK(weivel_get_service(core, plugin, "clock", &got, NULL) == WEIVEL_STATUS_OUT_OF_MEMORY);
    CHECK(got == 0);
    forget(core);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REVOKED) == 0);
    weivel_core_destroy(core);
}

static void a_want_that_cannot_be_held_is_not_settled(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"calendar", NULL, {"clock"}, {NULL}, {NULL}};
    weivel_handle plugin = open_spec(core, &spec);
    register_service(core, WEIVEL_HOST, "clock");
    weivel_census before;
    weivel_core_census(core, &before);
    fail_next_holding(core);
    weivel_handle want = 5;
    forget(core);
    CHECK(weivel_want_service(core, plugin, "clock", NULL, &want, NULL) == WEIVEL_STATUS_OUT_OF_MEMORY);
    CHECK(want == 0);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_WANT_SETTLED) == 0);
    weivel_census after;
    weivel_core_census(core, &after);
    CHECK(same_live_counts(&before, &after));
    weivel_core_destroy(core);
}

static void an_arrival_that_cannot_be_held_leaves_the_want_waiting(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"calendar", NULL, {"clock"}, {NULL}, {NULL}};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_handle want = want_untimed(core, plugin, "clock");
    fail_next_holding(core);
    weivel_handle first = register_service(core, WEIVEL_HOST, "clock");
    drain(core);
    CHECK(settlement_of(want) == NULL);
    CHECK(nth_of(WEIVEL_EVENT_LOG, 0)->level == WEIVEL_LOG_ERROR);
    forget(core);
    CHECK(weivel_unregister(core, first, NULL) == WEIVEL_STATUS_OK);
    weivel_handle second = register_service(core, WEIVEL_HOST, "clock");
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REVOKED) == 0);
    CHECK(settlement_of(want) != NULL && settlement_of(want)->implementation == second);
    weivel_core_destroy(core);
}

static void a_watch_that_cannot_hold_does_not_hear(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"calendar", NULL, {"clock"}, {NULL}, {NULL}};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, plugin, "clock", &watch, NULL) == WEIVEL_STATUS_OK);
    fail_next_holding(core);
    weivel_handle registration = register_service(core, WEIVEL_HOST, "clock");
    drain(core);
    CHECK(heard_by(watch, WEIVEL_CHANGE_REGISTERED) == 0);
    CHECK(nth_of(WEIVEL_EVENT_LOG, 0)->level == WEIVEL_LOG_ERROR);
    forget(core);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REVOKED) == 0);
    weivel_core_destroy(core);
}

int main(void) {
    churn_leaves_nothing_behind();
    a_slot_goes_when_nothing_uses_it();
    a_get_that_cannot_be_held_hands_nothing_over();
    a_want_that_cannot_be_held_is_not_settled();
    an_arrival_that_cannot_be_held_leaves_the_want_waiting();
    a_watch_that_cannot_hold_does_not_hear();
    return check_summary("lifecycle_test");
}
