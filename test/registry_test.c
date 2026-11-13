#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <weivel/core.h>

#include "registry_support.h"

static const char *const accounts[] = {"accounts"};

static weivel_handle consumer_of(weivel_core *core, const char *identity, const char *id) {
    plugin_spec spec = {identity, NULL, {id}, {NULL}, {NULL}};
    return open_spec(core, &spec);
}

static weivel_handle provider_of(weivel_core *core, const char *identity, const char *id) {
    plugin_spec spec = {identity, NULL, {NULL}, {NULL}, {id}};
    return open_spec(core, &spec);
}

/** The want vector's run: ask at 0, register at `register_at` unless negative, advance to the horizon. */
static const seen *want_vector(weivel_core *core, int64_t register_at, bool timed, int64_t timeout,
                               int64_t horizon, int64_t *settled_at) {
    weivel_handle consumer = consumer_of(core, "consumer", "accounts");
    weivel_handle provider = provider_of(core, "provider", "accounts");
    forget(core);
    weivel_handle want = timed ? want_within(core, consumer, "accounts", 0, timeout)
                               : want_untimed(core, consumer, "accounts");
    *settled_at = -1;
    for (int64_t now = 0; now <= horizon; now += 250) {
        CHECK(weivel_advance(core, now, NULL) == WEIVEL_STATUS_OK);
        if (now == register_at) {
            register_service(core, provider, "accounts");
        }
        drain(core);
        if (*settled_at < 0 && settlement_of(want) != NULL) {
            *settled_at = now;
        }
    }
    return settlement_of(want);
}

static void want_settles_when_a_provider_arrives_later(void) {
    weivel_core *core = core_knowing(accounts, 1);
    int64_t at;
    const seen *settled = want_vector(core, 5000, false, 0, 10000, &at);
    CHECK(settled != NULL && settled->implementation != 0);
    CHECK(at == 5000);
    weivel_core_destroy(core);
}

static void want_times_out_only_when_asked(void) {
    weivel_core *core = core_knowing(accounts, 1);
    int64_t at;
    const seen *settled = want_vector(core, -1, true, 1000, 10000, &at);
    CHECK(settled != NULL && settled->implementation == 0);
    if (settled == NULL) {
        return;
    }
    CHECK(settled->code == WEIVEL_REFUSAL_TIMED_OUT);
    CHECK(at == 1000);
    check_text(settled->detail, "waited 1000ms for service \"accounts\" and nothing registered it", "detail");
    check_text(settled->fix, "install a plugin that provides \"accounts\", or get it and carry on without it", "fix");
    weivel_core_destroy(core);
}

static void want_waits_without_a_deadline_when_nothing_provides(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle consumer = consumer_of(core, "consumer", "accounts");
    weivel_handle want = want_untimed(core, consumer, "accounts");
    int64_t deadline = 7;
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_NONE);
    CHECK(weivel_advance(core, 3600000, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_advance(core, INT64_MAX, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(settlement_of(want) == NULL);
    weivel_core_destroy(core);
}

static void a_timed_want_settles_exactly_once_with_the_service(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle consumer = consumer_of(core, "consumer", "accounts");
    weivel_handle provider = provider_of(core, "provider", "accounts");
    weivel_handle want = want_within(core, consumer, "accounts", 100, 1000);
    int64_t deadline = 0;
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_OK);
    CHECK(deadline == 1100);
    CHECK(weivel_advance(core, 1099, NULL) == WEIVEL_STATUS_OK);
    weivel_handle registration = register_service(core, provider, "accounts");
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_NONE);
    CHECK(weivel_advance(core, 5000, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    register_service(core, provider, "accounts");
    drain(core);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->implementation == registration && settled->plugin == consumer);
    CHECK(count_kind(WEIVEL_EVENT_WANT_SETTLED) == 1);
    weivel_core_destroy(core);
}

static void a_timed_out_want_never_settles_again(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle consumer = consumer_of(core, "consumer", "accounts");
    weivel_handle provider = provider_of(core, "provider", "accounts");
    weivel_handle late = want_within(core, consumer, "accounts", 0, 50);
    weivel_handle early = want_within(core, consumer, "accounts", 0, 20);
    int64_t deadline = 0;
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_OK && deadline == 20);
    CHECK(weivel_advance(core, 20, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_OK && deadline == 50);
    CHECK(weivel_advance(core, 60, NULL) == WEIVEL_STATUS_OK);
    register_service(core, provider, "accounts");
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_WANT_SETTLED) == 2);
    CHECK(nth_of(WEIVEL_EVENT_WANT_SETTLED, 0)->subject == early);
    CHECK(nth_of(WEIVEL_EVENT_WANT_SETTLED, 1)->subject == late);
    CHECK(settlement_of(late) != NULL && settlement_of(late)->code == WEIVEL_REFUSAL_TIMED_OUT);
    CHECK(weivel_unregister(core, late, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_core_destroy(core);
}

static void a_zero_timeout_waits_for_the_advance_at_its_now(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle consumer = consumer_of(core, "consumer", "accounts");
    weivel_handle want = want_within(core, consumer, "accounts", 40, 0);
    CHECK(weivel_advance(core, 39, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(settlement_of(want) == NULL);
    CHECK(weivel_advance(core, 40, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(settlement_of(want) != NULL);
    weivel_core_destroy(core);
}

static void a_withdrawn_want_never_settles(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle consumer = consumer_of(core, "consumer", "accounts");
    weivel_handle provider = provider_of(core, "provider", "accounts");
    weivel_handle want = want_within(core, consumer, "accounts", 0, 10);
    CHECK(weivel_unregister(core, want, NULL) == WEIVEL_STATUS_OK);
    int64_t deadline;
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_NONE);
    register_service(core, provider, "accounts");
    CHECK(weivel_advance(core, 100, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_WANT_SETTLED) == 0);
    weivel_core_destroy(core);
}

static void wants_settle_in_the_order_they_asked(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle first = consumer_of(core, "first", "accounts");
    weivel_handle second = consumer_of(core, "second", "accounts");
    weivel_handle provider = provider_of(core, "provider", "accounts");
    weivel_handle asked_first = want_untimed(core, second, "accounts");
    weivel_handle asked_second = want_untimed(core, first, "accounts");
    register_service(core, provider, "accounts");
    drain(core);
    CHECK(nth_of(WEIVEL_EVENT_WANT_SETTLED, 0)->subject == asked_first);
    CHECK(nth_of(WEIVEL_EVENT_WANT_SETTLED, 1)->subject == asked_second);
    weivel_core_destroy(core);
}

static void a_registration_answers_the_handle_that_unregisters_it(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle consumer = consumer_of(core, "consumer", "accounts");
    weivel_handle provider = provider_of(core, "provider", "accounts");
    weivel_handle registration = register_service(core, provider, "accounts");
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, consumer, "accounts", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(got == registration);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_get_service(core, consumer, "accounts", &got, NULL) == WEIVEL_STATUS_NONE);
    CHECK(got == 0);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_core_destroy(core);
}

static void a_revoked_handle_says_so(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle consumer = consumer_of(core, "consumer", "accounts");
    weivel_handle provider = provider_of(core, "provider", "accounts");
    weivel_handle registration = register_service(core, provider, "accounts");
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, consumer, "accounts", &got, NULL) == WEIVEL_STATUS_OK);
    forget(core);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(consumer, got) == 1);
    weivel_core_destroy(core);
}

static void a_revocation_reaches_every_holder_once(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle getter = consumer_of(core, "getter", "accounts");
    weivel_handle wanter = consumer_of(core, "wanter", "accounts");
    weivel_handle watcher = consumer_of(core, "watcher", "accounts");
    weivel_handle bystander = consumer_of(core, "bystander", "accounts");
    weivel_handle provider = provider_of(core, "provider", "accounts");
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, watcher, "accounts", &watch, NULL) == WEIVEL_STATUS_OK);
    want_untimed(core, wanter, "accounts");
    weivel_handle registration = register_service(core, provider, "accounts");
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, getter, "accounts", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_get_service(core, getter, "accounts", &got, NULL) == WEIVEL_STATUS_OK);
    want_untimed(core, getter, "accounts");
    forget(core);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(getter, registration) == 1);
    CHECK(revocations_to(wanter, registration) == 1);
    CHECK(revocations_to(watcher, registration) == 1);
    CHECK(revocations_to(bystander, registration) == 0);
    CHECK(count_kind(WEIVEL_EVENT_REVOKED) == 3);
    CHECK(heard_by(watch, WEIVEL_CHANGE_UNREGISTERED) == 1);
    CHECK(the_journal.items[the_journal.count - 1].kind == WEIVEL_EVENT_WATCH);
    weivel_core_destroy(core);
}

static void a_watch_hears_future_changes_only(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle watcher = consumer_of(core, "watcher", "accounts");
    weivel_handle provider = provider_of(core, "provider", "accounts");
    weivel_handle before = register_service(core, provider, "accounts");
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, watcher, "accounts", &watch, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(heard_by(watch, WEIVEL_CHANGE_REGISTERED) == 0);
    forget(core);
    CHECK(weivel_unregister(core, before, NULL) == WEIVEL_STATUS_OK);
    weivel_handle after = register_service(core, provider, "accounts");
    drain(core);
    CHECK(heard_by(watch, WEIVEL_CHANGE_UNREGISTERED) == 1);
    CHECK(heard_by(watch, WEIVEL_CHANGE_REGISTERED) == 1);
    const seen *heard = nth_of(WEIVEL_EVENT_WATCH, 1);
    CHECK(heard->implementation == after && heard->plugin == watcher);
    CHECK(weivel_unregister(core, watch, NULL) == WEIVEL_STATUS_OK);
    forget(core);
    CHECK(weivel_unregister(core, after, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_WATCH) == 0);
    weivel_core_destroy(core);
}

static void releasing_a_consumer_rejects_what_it_still_waits_for(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle a = consumer_of(core, "a", "b:z");
    weivel_handle want = want_untimed(core, a, "b:z");
    CHECK(weivel_release_plugin(core, a, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->implementation == 0 && settled->code == WEIVEL_REFUSAL_STOPPED);
    if (settled == NULL) {
        return;
    }
    check_text(settled->detail, "stopped while waiting for service \"b:z\"", "detail");
    check_text(settled->fix, "provide \"b:z\" before this plugin is stopped, or get it and carry on without it",
               "fix");
    weivel_core_destroy(core);
}

static void releasing_one_consumer_leaves_another_standing(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle a = provider_of(core, "a", "a:x");
    weivel_handle b = provider_of(core, "b", "b:y");
    plugin_spec observer_spec = {"observer", NULL, {"a:x", "b:y"}, {NULL}, {NULL}};
    weivel_handle observer = open_spec(core, &observer_spec);
    register_service(core, a, "a:x");
    weivel_handle kept = register_service(core, b, "b:y");
    CHECK(weivel_release_plugin(core, a, NULL) == WEIVEL_STATUS_OK);
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, observer, "a:x", &got, NULL) == WEIVEL_STATUS_NONE);
    CHECK(weivel_get_service(core, observer, "b:y", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(got == kept);
    weivel_core_destroy(core);
}

static void releasing_a_plugin_takes_everything_in_one_call(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec leaving_spec = {"leaving", NULL, {"other:s", "other:t"}, {NULL}, {"leaving:s"}};
    plugin_spec other_spec = {"other", NULL, {"leaving:s"}, {NULL}, {"other:s", "other:t"}};
    weivel_handle leaving = open_spec(core, &leaving_spec);
    weivel_handle other = open_spec(core, &other_spec);
    weivel_handle offered = register_service(core, leaving, "leaving:s");
    weivel_handle held = register_service(core, other, "other:s");
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, other, "leaving:s", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_get_service(core, leaving, "other:s", &got, NULL) == WEIVEL_STATUS_OK);
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, leaving, "other:t", &watch, NULL) == WEIVEL_STATUS_OK);
    weivel_handle other_watch = 0;
    CHECK(weivel_watch_service(core, other, "leaving:s", &other_watch, NULL) == WEIVEL_STATUS_OK);
    weivel_handle want = want_within(core, leaving, "other:t", 0, 100);
    forget(core);
    CHECK(weivel_release_plugin(core, leaving, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(other, offered) == 1);
    CHECK(heard_by(other_watch, WEIVEL_CHANGE_UNREGISTERED) == 1);
    CHECK(settlement_of(want) != NULL && settlement_of(want)->code == WEIVEL_REFUSAL_STOPPED);
    CHECK(revocations_to(leaving, held) == 1);
    CHECK(count_kind(WEIVEL_EVENT_DEADLINE) == 1 && nth_of(WEIVEL_EVENT_DEADLINE, 0)->deadline_set == 0);
    CHECK(the_journal.count == 5);
    int64_t deadline;
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_NONE);
    CHECK(weivel_unregister(core, watch, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_unregister(core, offered, NULL) == WEIVEL_STATUS_REFUSED);
    forget(core);
    register_service(core, other, "other:t");
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_WATCH) == 0);
    CHECK(count_kind(WEIVEL_EVENT_WANT_SETTLED) == 0);
    weivel_core_destroy(core);
}

static void a_quarantined_plugin_keeps_its_name_and_reopens(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}};
    weivel_handle plugin = open_spec(core, &spec);
    register_service(core, plugin, "calendar:events");
    CHECK(weivel_quarantine_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_quarantine_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "calendar";
    weivel_handle again = 0;
    CHECK(weivel_register_manifest(core, &manifest, &again, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_handle registration = 0;
    CHECK(weivel_register_service(core, plugin, "calendar:events", &registration, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    register_service(core, plugin, "calendar:events");
    weivel_core_destroy(core);
}

static void a_released_name_can_be_registered_again(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"forge:7", "calendar", {NULL}, {NULL}, {"calendar:events"}};
    weivel_handle plugin = open_spec(core, &spec);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    weivel_handle again = open_spec(core, &spec);
    CHECK(again != plugin && again != 0);
    forget(core);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_quarantine_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_PLUGIN_STATE);
    check_text(nth_of(WEIVEL_EVENT_REFUSAL, 0)->detail, "the plugin \"calendar\" was released", "detail");
    weivel_core_destroy(core);
}

/** Stopping a plugin never opened does not open it: its gets and watches are still refused as never opened. */
static void a_plugin_never_opened_stays_fenced_when_stopped(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec quarantined_spec = {"forge:3", "calendar", {"calendar:events"}, {NULL}, {NULL}};
    plugin_spec released_spec = {"forge:4", "notes", {"calendar:events"}, {NULL}, {NULL}};
    weivel_handle quarantined = register_spec(core, &quarantined_spec);
    weivel_handle released = register_spec(core, &released_spec);
    register_service(core, WEIVEL_HOST, "calendar:events");
    CHECK(weivel_quarantine_plugin(core, quarantined, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_release_plugin(core, released, NULL) == WEIVEL_STATUS_OK);
    forget(core);
    weivel_handle watch = 1;
    CHECK(weivel_watch_service(core, quarantined, "calendar:events", &watch, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(watch == 0);
    weivel_handle got = 1;
    CHECK(weivel_get_service(core, released, "calendar:events", &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(got == 0);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REFUSAL) == 2 && count_kind(WEIVEL_EVENT_LOG) == 0);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_PLUGIN_STATE);
    check_text(nth_of(WEIVEL_EVENT_REFUSAL, 0)->detail, "the plugin \"calendar\" has not been opened", "detail");
    check_text(nth_of(WEIVEL_EVENT_REFUSAL, 1)->detail, "the plugin \"notes\" has not been opened", "detail");
    weivel_core_destroy(core);
}

static void a_plugin_must_be_opened_before_it_reaches(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"calendar", NULL, {"calendar:events"}, {NULL}, {"calendar:events"}};
    weivel_handle plugin = register_spec(core, &spec);
    weivel_handle handle = 1;
    CHECK(weivel_get_service(core, plugin, "calendar:events", &handle, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_want_service(core, plugin, "calendar:events", NULL, &handle, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_watch_service(core, plugin, "calendar:events", &handle, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_register_service(core, plugin, "calendar:events", &handle, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(handle == 0);
    weivel_service_list list = WEIVEL_INIT(weivel_service_list);
    CHECK(weivel_service_ids(core, plugin, &list, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REFUSAL) == 5);
    const seen *first = nth_of(WEIVEL_EVENT_REFUSAL, 0);
    CHECK(first->code == WEIVEL_REFUSAL_PLUGIN_STATE);
    check_text(first->detail, "the plugin \"calendar\" has not been opened", "detail");
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_core_destroy(core);
}

static weivel_manifest bare_manifest(const char *import_name, const char *identity) {
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = import_name;
    manifest.identity = identity;
    return manifest;
}

static void an_identity_is_held_by_one_plugin(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec first = {"forge:2", "calendar", {NULL}, {NULL}, {NULL}};
    weivel_handle holder = register_spec(core, &first);
    weivel_manifest claimant = bare_manifest("agenda", "forge:2");
    weivel_handle refused = 7;
    forget(core);
    CHECK(weivel_register_manifest(core, &claimant, &refused, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(refused == 0);
    drain(core);
    const seen *taken = nth_of(WEIVEL_EVENT_REFUSAL, 0);
    CHECK(taken->code == WEIVEL_REFUSAL_IDENTITY_TAKEN);
    check_text(taken->detail, "a plugin with the identity \"forge:2\" is already registered", "detail");
    check_text(taken->fix, "give each plugin its own identity, or release the plugin that holds it", "fix");
    weivel_manifest by_name = bare_manifest("forge:2", NULL);
    CHECK(weivel_register_manifest(core, &by_name, &refused, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_release_plugin(core, holder, NULL) == WEIVEL_STATUS_OK);
    weivel_handle admitted = 0;
    CHECK(weivel_register_manifest(core, &claimant, &admitted, NULL) == WEIVEL_STATUS_OK);
    weivel_manifest empty = bare_manifest("notes", "");
    forget(core);
    CHECK(weivel_register_manifest(core, &empty, &refused, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_MALFORMED_MANIFEST);
    check_text(nth_of(WEIVEL_EVENT_REFUSAL, 0)->detail, "the manifest of \"notes\" gives an empty identity", "detail");
    weivel_core_destroy(core);
}

static void a_reopened_plugin_keeps_the_identity_consumers_bind_to(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec provider_spec = {"forge:2", "calendar", {NULL}, {NULL}, {"calendar:events"}};
    plugin_spec consumer_spec = {"agenda", NULL, {"calendar:events"}, {"forge:2"}, {NULL}};
    weivel_handle provider = open_spec(core, &provider_spec);
    weivel_handle consumer = open_spec(core, &consumer_spec);
    CHECK(weivel_quarantine_plugin(core, provider, NULL, NULL) == WEIVEL_STATUS_OK);
    weivel_plugin_options options = WEIVEL_INIT(weivel_plugin_options);
    options.payload = 99;
    CHECK(weivel_open_plugin(core, provider, &options, NULL) == WEIVEL_STATUS_OK);
    weivel_handle registration = register_service(core, provider, "calendar:events");
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, consumer, "calendar:events", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(got == registration);
    weivel_core_destroy(core);
}

static void a_manifest_of_the_previous_size_is_named_by_its_import_name(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_service_offer offer = WEIVEL_INIT(weivel_service_offer);
    offer.id = "calendar:events";
    weivel_manifest manifest = bare_manifest("calendar", "planted beyond the size");
    manifest.size = (uint32_t)offsetof(weivel_manifest, identity);
    manifest.services.provides.items = &offer;
    manifest.services.provides.count = 1;
    weivel_handle provider = 0;
    CHECK(weivel_register_manifest(core, &manifest, &provider, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, provider, NULL, NULL) == WEIVEL_STATUS_OK);
    plugin_spec consumer_spec = {"agenda", NULL, {"calendar:events"}, {"calendar"}, {NULL}};
    weivel_handle consumer = open_spec(core, &consumer_spec);
    weivel_handle registration = register_service(core, provider, "calendar:events");
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, consumer, "calendar:events", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(got == registration);
    weivel_core_destroy(core);
}

static void a_forgotten_plugin_answers_stale(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec owner_spec = {"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}};
    plugin_spec leaving_spec = {"notes", NULL, {"calendar:events"}, {NULL}, {NULL}};
    weivel_handle owner = open_spec(core, &owner_spec);
    weivel_handle leaving = open_spec(core, &leaving_spec);
    weivel_handle registration = register_service(core, owner, "calendar:events");
    forget(core);
    CHECK(weivel_forget_plugin(core, leaving, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_PLUGIN_STATE);
    check_text(nth_of(WEIVEL_EVENT_REFUSAL, 0)->detail, "the plugin \"notes\" is not released", "detail");
    CHECK(weivel_quarantine_plugin(core, leaving, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_forget_plugin(core, leaving, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_release_plugin(core, leaving, NULL) == WEIVEL_STATUS_OK);
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, leaving, "calendar:events", &got, NULL) == WEIVEL_STATUS_OK);
    forget(core);
    CHECK(weivel_forget_plugin(core, leaving, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(leaving, registration) == 1);
    forget(core);
    CHECK(weivel_get_service(core, leaving, "calendar:events", &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_forget_plugin(core, leaving, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_STALE_HANDLE);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 1)->code == WEIVEL_REFUSAL_STALE_HANDLE);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REVOKED) == 0);
    CHECK(weivel_forget_plugin(NULL, leaving, NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_core_destroy(core);
}

static void a_plugin_holding_its_own_registration_is_released(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}};
    plugin_spec watcher_spec = {"agenda", NULL, {"calendar:events"}, {NULL}, {NULL}};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_handle watcher = open_spec(core, &watcher_spec);
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, watcher, "calendar:events", &watch, NULL) == WEIVEL_STATUS_OK);
    weivel_handle registration = register_service(core, plugin, "calendar:events");
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, plugin, "calendar:events", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(got == registration);
    forget(core);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(plugin, registration) == 1);
    CHECK(revocations_to(watcher, registration) == 1);
    CHECK(heard_by(watch, WEIVEL_CHANGE_UNREGISTERED) == 1);
    CHECK(the_journal.count == 3);
    CHECK(weivel_forget_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

/** Deadlines set out of order, some withdrawn, come due strictly in time order, and the next one is always exact. */
static void deadlines_come_due_in_order_whatever_is_withdrawn(void) {
    enum { WANTS = 200 };
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle consumer = consumer_of(core, "consumer", "accounts");
    weivel_handle wants[WANTS];
    for (uint32_t index = 0; index < WANTS; index++) {
        wants[index] = want_within(core, consumer, "accounts", 0, (int64_t)((index * 37) % WANTS) * 10 + 10);
    }
    for (uint32_t index = 0; index < WANTS; index += 3) {
        CHECK(weivel_unregister(core, wants[index], NULL) == WEIVEL_STATUS_OK);
    }
    forget(core);
    int64_t previous = 0;
    uint32_t settled = 0;
    int64_t deadline = 0;
    while (weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_OK) {
        CHECK(deadline > previous);
        CHECK(weivel_advance(core, deadline - 1, NULL) == WEIVEL_STATUS_OK);
        drain(core);
        CHECK(count_kind(WEIVEL_EVENT_WANT_SETTLED) == 0);
        CHECK(weivel_advance(core, deadline, NULL) == WEIVEL_STATUS_OK);
        drain(core);
        CHECK(count_kind(WEIVEL_EVENT_WANT_SETTLED) == 1);
        settled += count_kind(WEIVEL_EVENT_WANT_SETTLED);
        the_journal.count = 0;
        previous = deadline;
        if (settled > WANTS) {
            break;
        }
    }
    CHECK(settled == WANTS - (WANTS + 2) / 3);
    weivel_core_destroy(core);
}

static void hands_back_the_payload_it_was_given(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"config-ledger", NULL, {NULL}, {NULL}, {NULL}};
    weivel_handle plugin = register_spec(core, &spec);
    static int manifest_object;
    uint64_t token = (uint64_t)(uintptr_t)&manifest_object;
    uint64_t answered = 1;
    CHECK(weivel_plugin_payload(core, plugin, &answered, NULL) == WEIVEL_STATUS_OK);
    CHECK(answered == 0);
    weivel_plugin_options options = WEIVEL_INIT(weivel_plugin_options);
    options.payload = token;
    CHECK(weivel_open_plugin(core, plugin, &options, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_plugin_payload(core, plugin, &answered, NULL) == WEIVEL_STATUS_OK);
    CHECK((void *)(uintptr_t)answered == (void *)&manifest_object);
    CHECK(weivel_plugin_payload(core, plugin, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_core_destroy(core);
}

static void check_unbound(const char *id, const char *provider, uint32_t expected_count) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"agenda", NULL, {id}, {provider}, {NULL}};
    weivel_handle plugin = register_spec(core, &spec);
    weivel_service_list list = WEIVEL_INIT(weivel_service_list);
    CHECK(weivel_unbound_services(core, plugin, &list, NULL) == WEIVEL_STATUS_OK);
    CHECK(list.ids.count == expected_count);
    if (expected_count == 1) {
        check_text(list.ids.items[0], id, "unbound");
    }
    CHECK(weivel_service_list_release(&list) == WEIVEL_STATUS_OK);
    CHECK(list.storage == NULL && list.ids.count == 0);
    weivel_core_destroy(core);
}

static void unbound_lists_a_namespaced_service_with_no_provider(void) {
    check_unbound("calendar:events", NULL, 1);
}

static void unbound_omits_a_bare_shared_service(void) {
    check_unbound("accounts", NULL, 0);
}

static void unbound_omits_a_service_bound_to_a_provider(void) {
    check_unbound("calendar:events", "forge:2", 0);
}

static void the_host_owns_its_services(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle consumer = consumer_of(core, "consumer", "clock");
    plugin_spec squatter_spec = {"squatter", NULL, {NULL}, {NULL}, {NULL}};
    weivel_handle squatter = open_spec(core, &squatter_spec);
    weivel_handle first = register_service(core, WEIVEL_HOST, "clock");
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, consumer, "clock", &got, NULL) == WEIVEL_STATUS_OK && got == first);
    weivel_handle refused = 0;
    forget(core);
    CHECK(weivel_register_service(core, squatter, "clock", &refused, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_release_plugin(core, squatter, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    check_text(nth_of(WEIVEL_EVENT_REFUSAL, 0)->detail,
               "cannot register service \"clock\", which is outside this plugin's namespace", "detail");
    weivel_handle second = register_service(core, WEIVEL_HOST, "clock");
    drain(core);
    CHECK(second != first);
    CHECK(revocations_to(consumer, first) == 1);
    CHECK(weivel_get_service(core, consumer, "clock", &got, NULL) == WEIVEL_STATUS_OK && got == second);
    weivel_core_destroy(core);
}

static void a_plugin_cannot_take_an_id_the_host_holds(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle plugin = provider_of(core, "provider", "accounts");
    register_service(core, WEIVEL_HOST, "accounts");
    forget(core);
    weivel_handle refused = 0;
    CHECK(weivel_register_service(core, plugin, "accounts", &refused, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    const seen *taken = nth_of(WEIVEL_EVENT_REFUSAL, 0);
    CHECK(taken->code == WEIVEL_REFUSAL_SERVICE_TAKEN);
    check_text(taken->detail, "service \"accounts\" is already registered by the host", "detail");
    check_text(taken->fix, "disable one of the two plugins, or have each register its own namespaced id", "fix");
    weivel_core_destroy(core);
}

static void a_bound_consumer_never_accepts_the_host(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"agenda", NULL, {"clock"}, {"forge:2"}, {NULL}};
    weivel_handle consumer = open_spec(core, &spec);
    weivel_handle want = want_untimed(core, consumer, "clock");
    register_service(core, WEIVEL_HOST, "clock");
    drain(core);
    CHECK(settlement_of(want) == NULL);
    const seen *mismatch = nth_of(WEIVEL_EVENT_REACH_REFUSED, 0);
    CHECK(mismatch != NULL && mismatch->code == WEIVEL_REFUSAL_WRONG_PROVIDER);
    mentions(mismatch->detail, "but the host provides");
    weivel_core_destroy(core);
}

static void a_plugin_registers_only_in_its_namespace_or_a_well_known_id(void) {
    static const char *const known[] = {"accounts", "locale"};
    weivel_core *core = core_knowing(known, 2);
    plugin_spec spec = {"forge:9", "calendar", {NULL}, {NULL}, {NULL}};
    weivel_handle plugin = open_spec(core, &spec);
    register_service(core, plugin, "calendar:events");
    register_service(core, plugin, "accounts");
    forget(core);
    weivel_handle refused = 0;
    CHECK(weivel_register_service(core, plugin, "notes:events", &refused, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_register_service(core, plugin, "calendar:", &refused, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_register_service(core, plugin, ":events", &refused, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_register_service(core, plugin, "calendar:events", &refused, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    const seen *outside = nth_of(WEIVEL_EVENT_REFUSAL, 0);
    CHECK(outside->code == WEIVEL_REFUSAL_OUTSIDE_NAMESPACE);
    check_text(outside->fix, "register it as \"calendar:events\", or as one of the well-known ids: accounts, locale",
               "fix");
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 3)->code == WEIVEL_REFUSAL_SERVICE_TAKEN);
    check_text(nth_of(WEIVEL_EVENT_REFUSAL, 3)->detail, "service \"calendar:events\" is already registered by forge:9",
               "detail");
    weivel_core_destroy(core);
}

static void options_of_the_previous_size_know_no_well_known_id(void) {
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.size = (uint32_t)offsetof(weivel_core_options, well_known_services);
    options.well_known_services.items = accounts;
    options.well_known_services.count = 1;
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    weivel_handle plugin = provider_of(core, "provider", "accounts");
    weivel_handle refused = 0;
    CHECK(weivel_register_service(core, plugin, "accounts", &refused, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_core_destroy(core);
    const char *holes[] = {"accounts", NULL};
    options.size = (uint32_t)sizeof options;
    options.well_known_services.items = holes;
    options.well_known_services.count = 2;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_MALFORMED);
}

static void the_well_known_ids_are_copied_at_create(void) {
    char name[] = "accounts";
    const char *items[] = {name};
    weivel_core *core = core_knowing(items, 1);
    name[0] = 'X';
    weivel_handle plugin = provider_of(core, "provider", "accounts");
    register_service(core, plugin, "accounts");
    weivel_core_destroy(core);
}

static void ids_follow_the_manifest_order(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"calendar", NULL, {"zeta", "alpha"}, {NULL}, {"calendar:events", "zeta"}};
    weivel_handle plugin = open_spec(core, &spec);
    register_service(core, WEIVEL_HOST, "alpha");
    register_service(core, WEIVEL_HOST, "zeta");
    register_service(core, plugin, "calendar:events");
    weivel_service_list list = WEIVEL_INIT(weivel_service_list);
    CHECK(weivel_service_ids(core, plugin, &list, NULL) == WEIVEL_STATUS_OK);
    CHECK(list.ids.count == 3);
    if (list.ids.count == 3) {
        check_text(list.ids.items[0], "zeta", "first");
        check_text(list.ids.items[1], "alpha", "second");
        check_text(list.ids.items[2], "calendar:events", "third");
    }
    CHECK(weivel_service_ids(core, plugin, &list, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_service_list_release(&list) == WEIVEL_STATUS_OK);
    weivel_service_list zeroed;
    memset(&zeroed, 0, sizeof zeroed);
    CHECK(weivel_service_list_release(&zeroed) == WEIVEL_STATUS_OK);
    CHECK(weivel_service_list_release(NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_core_destroy(core);
}

static void malformed_calls_change_nothing(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle plugin = consumer_of(core, "consumer", "accounts");
    weivel_handle handle = 9;
    CHECK(weivel_register_service(NULL, plugin, "accounts", &handle, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_register_service(core, plugin, NULL, &handle, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(handle == 0);
    CHECK(weivel_register_service(core, plugin, "", &handle, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_register_service(core, plugin, "accounts", NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_get_service(core, plugin, "", &handle, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_get_service(core, plugin, "accounts", NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_watch_service(core, plugin, NULL, &handle, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_want_service(core, plugin, "accounts", NULL, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_want_options options = WEIVEL_INIT(weivel_want_options);
    options.timed = 3;
    CHECK(weivel_want_service(core, plugin, "accounts", &options, &handle, NULL) == WEIVEL_STATUS_MALFORMED);
    options.timed = WEIVEL_CHOICE_YES;
    options.timeout_millis = -1;
    CHECK(weivel_want_service(core, plugin, "accounts", &options, &handle, NULL) == WEIVEL_STATUS_MALFORMED);
    options.size = 4;
    CHECK(weivel_want_service(core, plugin, "accounts", &options, &handle, NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_plugin_options small = WEIVEL_INIT(weivel_plugin_options);
    small.size = 4;
    CHECK(weivel_open_plugin(core, plugin, &small, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_unregister(NULL, plugin, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_release_plugin(NULL, plugin, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_quarantine_plugin(NULL, plugin, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_open_plugin(NULL, plugin, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_service_ids(core, plugin, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    forget(core);
    options.size = (uint32_t)sizeof options;
    options.timed = WEIVEL_CHOICE_NO;
    options.timeout_millis = -5;
    weivel_handle want = 0;
    CHECK(weivel_want_service(core, plugin, "accounts", &options, &want, NULL) == WEIVEL_STATUS_OK);
    int64_t deadline;
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_NONE);
    weivel_core_destroy(core);
}

static void stale_handles_are_refused_as_data(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle plugin = provider_of(core, "provider", "accounts");
    weivel_handle registration = register_service(core, plugin, "accounts");
    weivel_handle handle = 1;
    forget(core);
    CHECK(weivel_get_service(core, registration, "accounts", &handle, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_release_plugin(core, registration, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_unregister(core, plugin, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_unregister(core, 0, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_register_service(core, 12345, "accounts", &handle, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REFUSAL) == 5);
    for (uint32_t index = 0; index < 5; index++) {
        CHECK(nth_of(WEIVEL_EVENT_REFUSAL, index)->code == WEIVEL_REFUSAL_STALE_HANDLE);
    }
    weivel_core_destroy(core);
}

/**
 * Thousands of plugins in a chain, each providing one service and consuming the one before it, exercise every
 * table's growth: names, identities, slots and holdings.
 */
static void thousands_of_plugins_and_services_resolve(void) {
    enum { PLUGINS = 3000 };
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle *plugins = calloc(PLUGINS, sizeof *plugins);
    weivel_handle *registrations = calloc(PLUGINS, sizeof *registrations);
    weivel_handle *wants = calloc(PLUGINS, sizeof *wants);
    char name[32];
    char provided[48];
    char consumed[48];
    for (uint32_t index = 0; index < PLUGINS; index++) {
        snprintf(name, sizeof name, "p%u", index);
        snprintf(provided, sizeof provided, "p%u:s", index);
        snprintf(consumed, sizeof consumed, "p%u:s", (index + PLUGINS - 1) % PLUGINS);
        plugin_spec spec = {name, NULL, {consumed}, {NULL}, {provided}};
        plugins[index] = open_spec(core, &spec);
        wants[index] = want_untimed(core, plugins[index], consumed);
    }
    for (uint32_t index = 0; index < PLUGINS; index++) {
        snprintf(provided, sizeof provided, "p%u:s", index);
        registrations[index] = register_service(core, plugins[index], provided);
        drain(core);
        forget(core);
    }
    uint32_t served = 0;
    for (uint32_t index = 0; index < PLUGINS; index++) {
        snprintf(consumed, sizeof consumed, "p%u:s", (index + PLUGINS - 1) % PLUGINS);
        weivel_handle got = 0;
        served += weivel_get_service(core, plugins[index], consumed, &got, NULL) == WEIVEL_STATUS_OK &&
                  got == registrations[(index + PLUGINS - 1) % PLUGINS];
    }
    CHECK(served == PLUGINS);
    uint32_t revoked = 0;
    for (uint32_t index = 0; index < PLUGINS; index++) {
        CHECK(weivel_release_plugin(core, plugins[index], NULL) == WEIVEL_STATUS_OK);
        drain(core);
        revoked += count_kind(WEIVEL_EVENT_REVOKED);
        forget(core);
    }
    CHECK(revoked == PLUGINS);
    plugin_spec reused = {"p0", NULL, {NULL}, {NULL}, {"p0:s"}};
    weivel_handle again = open_spec(core, &reused);
    register_service(core, again, "p0:s");
    free(plugins);
    free(registrations);
    free(wants);
    weivel_core_destroy(core);
}

/**
 * The host asks as a plugin would, unfenced: it reaches any id, a plugin's own namespace included, holds what it is
 * handed and is revoked it like a plugin, and no plugin stopping ends its watches or wants.
 */
static void the_host_reaches_every_service_and_holds_like_a_plugin(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}};
    weivel_handle calendar = open_spec(core, &spec);
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, WEIVEL_HOST, "calendar:events", &watch, NULL) == WEIVEL_STATUS_OK);
    weivel_handle want = 0;
    CHECK(weivel_want_service(core, WEIVEL_HOST, "calendar:events", NULL, &want, NULL) == WEIVEL_STATUS_OK);
    weivel_handle unanswered = 0;
    CHECK(weivel_want_service(core, WEIVEL_HOST, "nobody:offers", NULL, &unanswered, NULL) == WEIVEL_STATUS_OK);
    weivel_handle got = 5;
    CHECK(weivel_get_service(core, WEIVEL_HOST, "calendar:events", &got, NULL) == WEIVEL_STATUS_NONE);
    CHECK(got == 0);
    drain(core);
    CHECK(the_journal.count == 0);
    weivel_handle registration = register_service(core, calendar, "calendar:events");
    CHECK(weivel_get_service(core, WEIVEL_HOST, "calendar:events", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(got == registration);
    drain(core);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->plugin == WEIVEL_HOST && settled->implementation == registration);
    CHECK(heard_by(watch, WEIVEL_CHANGE_REGISTERED) == 1);
    CHECK(nth_of(WEIVEL_EVENT_WATCH, 0)->plugin == WEIVEL_HOST);
    forget(core);
    CHECK(weivel_release_plugin(core, calendar, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(WEIVEL_HOST, registration) == 1);
    CHECK(heard_by(watch, WEIVEL_CHANGE_UNREGISTERED) == 1);
    CHECK(settlement_of(unanswered) == NULL);
    weivel_handle again = open_spec(core, &spec);
    forget(core);
    weivel_handle second = register_service(core, again, "calendar:events");
    drain(core);
    CHECK(heard_by(watch, WEIVEL_CHANGE_REGISTERED) == 1);
    CHECK(nth_of(WEIVEL_EVENT_WATCH, 0)->implementation == second);
    CHECK(weivel_unregister(core, watch, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_unregister(core, unanswered, NULL) == WEIVEL_STATUS_OK);
    forget(core);
    CHECK(weivel_unregister(core, second, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(WEIVEL_HOST, second) == 1);
    CHECK(count_kind(WEIVEL_EVENT_WATCH) == 0 && count_kind(WEIVEL_EVENT_WANT_SETTLED) == 0);
    weivel_ledger_snapshot snapshot = WEIVEL_INIT(weivel_ledger_snapshot);
    CHECK(weivel_ledger(core, &snapshot, NULL) == WEIVEL_STATUS_OK);
    CHECK(snapshot.row_count == 2 && snapshot.refusal_count == 0);
    weivel_ledger_snapshot_release(&snapshot);
    weivel_core_destroy(core);
}

int main(void) {
    the_host_reaches_every_service_and_holds_like_a_plugin();
    want_settles_when_a_provider_arrives_later();
    want_times_out_only_when_asked();
    want_waits_without_a_deadline_when_nothing_provides();
    a_timed_want_settles_exactly_once_with_the_service();
    a_timed_out_want_never_settles_again();
    a_zero_timeout_waits_for_the_advance_at_its_now();
    a_withdrawn_want_never_settles();
    wants_settle_in_the_order_they_asked();
    a_registration_answers_the_handle_that_unregisters_it();
    a_revoked_handle_says_so();
    a_revocation_reaches_every_holder_once();
    a_watch_hears_future_changes_only();
    releasing_a_consumer_rejects_what_it_still_waits_for();
    releasing_one_consumer_leaves_another_standing();
    releasing_a_plugin_takes_everything_in_one_call();
    a_quarantined_plugin_keeps_its_name_and_reopens();
    a_released_name_can_be_registered_again();
    a_plugin_must_be_opened_before_it_reaches();
    a_plugin_never_opened_stays_fenced_when_stopped();
    an_identity_is_held_by_one_plugin();
    a_reopened_plugin_keeps_the_identity_consumers_bind_to();
    a_manifest_of_the_previous_size_is_named_by_its_import_name();
    a_forgotten_plugin_answers_stale();
    a_plugin_holding_its_own_registration_is_released();
    deadlines_come_due_in_order_whatever_is_withdrawn();
    hands_back_the_payload_it_was_given();
    unbound_lists_a_namespaced_service_with_no_provider();
    unbound_omits_a_bare_shared_service();
    unbound_omits_a_service_bound_to_a_provider();
    the_host_owns_its_services();
    a_plugin_cannot_take_an_id_the_host_holds();
    a_bound_consumer_never_accepts_the_host();
    a_plugin_registers_only_in_its_namespace_or_a_well_known_id();
    options_of_the_previous_size_know_no_well_known_id();
    the_well_known_ids_are_copied_at_create();
    ids_follow_the_manifest_order();
    malformed_calls_change_nothing();
    stale_handles_are_refused_as_data();
    thousands_of_plugins_and_services_resolve();
    return check_summary("registry_test");
}
