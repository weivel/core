#include <stdbool.h>
#include <stdint.h>
#include <weivel/core.h>

#include "registry_support.h"

static const char *const well_known[] = {"accounts", "ledger"};

static weivel_core *a_core(void) {
    return core_knowing(well_known, 2);
}

static weivel_handle consumer_in(weivel_core *core) {
    plugin_spec spec = {"consumer", NULL, {"accounts", "ledger"}, {NULL}, {NULL}};
    return open_spec(core, &spec);
}

static weivel_handle provider_in(weivel_core *core) {
    plugin_spec spec = {"provider", NULL, {NULL}, {NULL}, {"accounts", "ledger"}};
    return open_spec(core, &spec);
}

/** Drains what the calls since the last step queued, so the journal holds only those. */
static void step(weivel_core *core) {
    the_journal.count = 0;
    drain(core);
}

/** What weivel_next_deadline answers now, as an announcement would carry it. */
static weivel_deadline_event answered(weivel_core *core) {
    weivel_deadline_event now = {0, 0};
    weivel_status status = weivel_next_deadline(core, &now.at, NULL);
    CHECK(status == WEIVEL_STATUS_OK || status == WEIVEL_STATUS_NONE);
    if (status == WEIVEL_STATUS_NONE) {
        now.at = 0;
    }
    now.set = status == WEIVEL_STATUS_OK ? 1u : 0u;
    return now;
}

/**
 * The calls since the last step announced exactly one deadline, `set` and `at`, after every other event they queued,
 * and weivel_next_deadline answers the same.
 */
static void expect_announced(weivel_core *core, uint32_t set, int64_t at) {
    step(core);
    CHECK(count_kind(WEIVEL_EVENT_DEADLINE) == 1);
    CHECK(the_journal.count > 0 && the_journal.items[the_journal.count - 1].kind == WEIVEL_EVENT_DEADLINE);
    const seen *announced = nth_of(WEIVEL_EVENT_DEADLINE, 0);
    CHECK(announced->deadline_set == set);
    CHECK(announced->deadline == at);
    weivel_deadline_event now = answered(core);
    CHECK(now.set == set && now.at == at);
}

static void expect_silent(weivel_core *core) {
    step(core);
    CHECK(count_kind(WEIVEL_EVENT_DEADLINE) == 0);
}

static void a_timed_want_announces_its_deadline(void) {
    weivel_core *core = a_core();
    weivel_handle consumer = consumer_in(core);
    expect_silent(core);
    want_within(core, consumer, "accounts", 100, 1000);
    expect_announced(core, 1, 1100);
    weivel_core_destroy(core);
}

static void an_earlier_want_moves_the_deadline_and_a_later_one_is_silent(void) {
    weivel_core *core = a_core();
    weivel_handle consumer = consumer_in(core);
    want_within(core, consumer, "accounts", 0, 1100);
    expect_announced(core, 1, 1100);
    want_within(core, consumer, "ledger", 0, 500);
    expect_announced(core, 1, 500);
    want_within(core, consumer, "accounts", 0, 2000);
    expect_silent(core);
    weivel_core_destroy(core);
}

static void a_settled_want_announces_the_next_and_the_last_announces_none(void) {
    weivel_core *core = a_core();
    weivel_handle consumer = consumer_in(core);
    weivel_handle provider = provider_in(core);
    want_within(core, consumer, "accounts", 0, 500);
    want_within(core, consumer, "ledger", 0, 1100);
    step(core);
    register_service(core, provider, "accounts");
    expect_announced(core, 1, 1100);
    register_service(core, provider, "ledger");
    expect_announced(core, 0, 0);
    weivel_core_destroy(core);
}

static void a_withdrawn_want_announces_the_next_and_the_last_announces_none(void) {
    weivel_core *core = a_core();
    weivel_handle consumer = consumer_in(core);
    weivel_handle early = want_within(core, consumer, "accounts", 0, 500);
    weivel_handle late = want_within(core, consumer, "ledger", 0, 1100);
    step(core);
    CHECK(weivel_unregister(core, early, NULL) == WEIVEL_STATUS_OK);
    expect_announced(core, 1, 1100);
    CHECK(weivel_unregister(core, late, NULL) == WEIVEL_STATUS_OK);
    expect_announced(core, 0, 0);
    weivel_core_destroy(core);
}

static void withdrawing_a_later_want_is_silent(void) {
    weivel_core *core = a_core();
    weivel_handle consumer = consumer_in(core);
    want_within(core, consumer, "accounts", 0, 500);
    weivel_handle late = want_within(core, consumer, "ledger", 0, 1100);
    step(core);
    CHECK(weivel_unregister(core, late, NULL) == WEIVEL_STATUS_OK);
    expect_silent(core);
    CHECK(answered(core).at == 500);
    weivel_core_destroy(core);
}

static void advancing_past_a_deadline_announces_the_next_after_the_settlement(void) {
    weivel_core *core = a_core();
    weivel_handle consumer = consumer_in(core);
    weivel_handle early = want_within(core, consumer, "accounts", 0, 500);
    want_within(core, consumer, "ledger", 0, 1100);
    step(core);
    CHECK(weivel_advance(core, 499, NULL) == WEIVEL_STATUS_OK);
    expect_silent(core);
    CHECK(weivel_advance(core, 500, NULL) == WEIVEL_STATUS_OK);
    expect_announced(core, 1, 1100);
    CHECK(settlement_of(early) != NULL && settlement_of(early)->code == WEIVEL_REFUSAL_TIMED_OUT);
    CHECK(weivel_advance(core, 5000, NULL) == WEIVEL_STATUS_OK);
    expect_announced(core, 0, 0);
    CHECK(weivel_advance(core, 9000, NULL) == WEIVEL_STATUS_OK);
    expect_silent(core);
    weivel_core_destroy(core);
}

static void an_untimed_want_is_silent(void) {
    weivel_core *core = a_core();
    weivel_handle consumer = consumer_in(core);
    want_untimed(core, consumer, "accounts");
    expect_silent(core);
    weivel_core_destroy(core);
}

static void releasing_a_consumer_announces_that_nothing_waits_on_time(void) {
    weivel_core *core = a_core();
    weivel_handle consumer = consumer_in(core);
    want_within(core, consumer, "accounts", 0, 500);
    step(core);
    CHECK(weivel_release_plugin(core, consumer, NULL) == WEIVEL_STATUS_OK);
    expect_announced(core, 0, 0);
    weivel_core_destroy(core);
}

/** An announcement not yet drained is replaced by the newer, so one call or many leave the current deadline alone. */
static void an_undrained_announcement_is_replaced_by_the_newer(void) {
    weivel_core *core = a_core();
    weivel_handle consumer = consumer_in(core);
    weivel_handle late = want_within(core, consumer, "accounts", 0, 1100);
    weivel_handle early = want_within(core, consumer, "ledger", 0, 500);
    expect_announced(core, 1, 500);
    CHECK(weivel_unregister(core, early, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_unregister(core, late, NULL) == WEIVEL_STATUS_OK);
    expect_announced(core, 0, 0);
    weivel_core_destroy(core);
}

/** A deadline that comes back to the one announced, before anything was drained, is still announced as it is now. */
static void a_deadline_moved_away_and_back_is_announced_as_it_is(void) {
    weivel_core *core = a_core();
    weivel_handle consumer = consumer_in(core);
    want_within(core, consumer, "accounts", 0, 1100);
    expect_announced(core, 1, 1100);
    weivel_handle early = want_within(core, consumer, "ledger", 0, 500);
    CHECK(weivel_unregister(core, early, NULL) == WEIVEL_STATUS_OK);
    expect_announced(core, 1, 1100);
    weivel_core_destroy(core);
}

static const weivel_number at_least_three = {(uint32_t)sizeof(weivel_number), "replicas", 3};

static weivel_core *a_core_knowing_replicas(void) {
    weivel_constraint_entry entry = WEIVEL_INIT(weivel_constraint_entry);
    entry.constraint_name = "replicas";
    entry.attribute_name = "replicas";
    entry.bound = WEIVEL_BOUND_LOWER;
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.constraint_vocabulary.items = &entry;
    options.constraint_vocabulary.count = 1;
    options.well_known_services.items = well_known;
    options.well_known_services.count = 2;
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    the_journal.count = 0;
    return core;
}

static weivel_handle picky_consumer_in(weivel_core *core) {
    weivel_service_want want = WEIVEL_INIT(weivel_service_want);
    want.id = "accounts";
    want.constraints.items = &at_least_three;
    want.constraints.count = 1;
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "picky";
    manifest.api = 1;
    manifest.services.consumes.items = &want;
    manifest.services.consumes.count = 1;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

/** A constrained want's poll ending moves its deadline later in weivel_advance, which takes nothing to reserve. */
static void a_poll_ending_announces_the_next_poll(void) {
    weivel_core *core = a_core_knowing_replicas();
    weivel_handle picky = picky_consumer_in(core);
    weivel_want_options options = WEIVEL_INIT(weivel_want_options);
    options.poll_millis = 100;
    weivel_handle want = 0;
    CHECK(weivel_want_service(core, picky, "accounts", &options, &want, NULL) == WEIVEL_STATUS_OK);
    expect_announced(core, 1, 100);
    CHECK(weivel_advance(core, 100, NULL) == WEIVEL_STATUS_OK);
    expect_announced(core, 1, 200);
    CHECK(weivel_unregister(core, want, NULL) == WEIVEL_STATUS_OK);
    expect_announced(core, 0, 0);
    weivel_core_destroy(core);
}

/** A queue made as small as it may be still holds every announcement beside the settlements it owes. */
static void the_smallest_queue_keeps_room_for_the_announcement(void) {
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.initial_event_capacity = 1;
    options.well_known_services.items = well_known;
    options.well_known_services.count = 2;
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    the_journal.count = 0;
    weivel_handle consumer = consumer_in(core);
    step(core);
    weivel_handle want = want_within(core, consumer, "accounts", 0, 500);
    CHECK(weivel_advance(core, 5000, NULL) == WEIVEL_STATUS_OK);
    step(core);
    CHECK(the_journal.count == 2);
    CHECK(the_journal.items[0].kind == WEIVEL_EVENT_WANT_SETTLED && the_journal.items[0].subject == want);
    CHECK(the_journal.items[1].kind == WEIVEL_EVENT_DEADLINE && the_journal.items[1].deadline_set == 0);
    weivel_core_destroy(core);
}

int main(void) {
    a_timed_want_announces_its_deadline();
    an_earlier_want_moves_the_deadline_and_a_later_one_is_silent();
    a_settled_want_announces_the_next_and_the_last_announces_none();
    a_withdrawn_want_announces_the_next_and_the_last_announces_none();
    withdrawing_a_later_want_is_silent();
    advancing_past_a_deadline_announces_the_next_after_the_settlement();
    an_untimed_want_is_silent();
    releasing_a_consumer_announces_that_nothing_waits_on_time();
    an_undrained_announcement_is_replaced_by_the_newer();
    a_deadline_moved_away_and_back_is_announced_as_it_is();
    a_poll_ending_announces_the_next_poll();
    the_smallest_queue_keeps_room_for_the_announcement();
    return check_summary("deadline_test");
}
