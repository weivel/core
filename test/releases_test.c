#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <weivel/core.h>

#include "core_internal.h"
#include "registry_support.h"

static const char *const accounts[] = {"accounts"};

static weivel_handle consumer_of(weivel_core *core, const char *identity) {
    plugin_spec spec = {identity, NULL, {"accounts"}, {NULL}, {NULL}};
    return open_spec(core, &spec);
}

static weivel_handle provider_of(weivel_core *core, const char *identity) {
    plugin_spec spec = {identity, NULL, {"accounts"}, {NULL}, {"accounts"}};
    return open_spec(core, &spec);
}

static weivel_handle register_awaiting(weivel_core *core, weivel_handle owner) {
    weivel_handle registration = 0;
    CHECK(weivel_register_service_awaiting_releases(core, owner, "accounts", &registration, NULL) ==
          WEIVEL_STATUS_OK);
    CHECK(registration != 0);
    return registration;
}

static void get_accounts(weivel_core *core, weivel_handle plugin) {
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, plugin, "accounts", &got, NULL) == WEIVEL_STATUS_OK);
}

static uint32_t releases_to(weivel_handle owner, weivel_handle registration) {
    uint32_t count = 0;
    for (uint32_t at = 0; at < the_journal.count; at++) {
        const seen *item = &the_journal.items[at];
        count += item->kind == WEIVEL_EVENT_HOLDINGS_RELEASED && item->plugin == owner &&
                 item->implementation == registration;
    }
    return count;
}

static uint32_t census_holdings(weivel_core *core) {
    weivel_census census;
    weivel_core_census(core, &census);
    return census.holdings + census.revoked_holdings;
}

static bool reservations_balance(weivel_core *core) {
    weivel_census census;
    weivel_core_census(core, &census);
    return census.reserved_events ==
           census.wants + census.holdings + census.capability_holdings + census.release_reports;
}

static void refused_as_stale(weivel_core *core, weivel_handle holder, weivel_handle registration) {
    forget(core);
    CHECK(weivel_release_holding(core, holder, registration, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REFUSAL) == 1);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_STALE_HANDLE);
    CHECK(count_kind(WEIVEL_EVENT_HOLDINGS_RELEASED) == 0);
}

static void the_owner_hears_once_after_the_last_release(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle provider = provider_of(core, "provider");
    weivel_handle first = consumer_of(core, "first");
    weivel_handle second = consumer_of(core, "second");
    weivel_handle registration = register_awaiting(core, provider);
    get_accounts(core, first);
    get_accounts(core, second);
    get_accounts(core, second);
    forget(core);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(first, registration) == 1);
    CHECK(revocations_to(second, registration) == 1);
    CHECK(count_kind(WEIVEL_EVENT_HOLDINGS_RELEASED) == 0);
    CHECK(census_holdings(core) == 2);
    CHECK(reservations_balance(core));
    forget(core);
    CHECK(weivel_release_holding(core, first, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(the_journal.count == 0);
    CHECK(weivel_release_holding(core, second, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(the_journal.count == 1);
    CHECK(releases_to(provider, registration) == 1);
    CHECK(census_holdings(core) == 0);
    CHECK(reservations_balance(core));
    refused_as_stale(core, second, registration);
    weivel_core_destroy(core);
}

static void the_owner_hears_at_once_when_nothing_held_it(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle provider = provider_of(core, "provider");
    weivel_handle watcher = consumer_of(core, "watcher");
    weivel_handle registration = register_awaiting(core, provider);
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, watcher, "accounts", &watch, NULL) == WEIVEL_STATUS_OK);
    CHECK(reservations_balance(core));
    forget(core);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(releases_to(provider, registration) == 1);
    CHECK(heard_by(watch, WEIVEL_CHANGE_UNREGISTERED) == 1);
    CHECK(the_journal.count == 2 && the_journal.items[1].kind == WEIVEL_EVENT_HOLDINGS_RELEASED);
    CHECK(reservations_balance(core));
    weivel_core_destroy(core);
}

static void unregister_with_one_holder(weivel_core *core, weivel_handle *provider, weivel_handle *holder,
                                       weivel_handle *registration) {
    *provider = provider_of(core, "provider");
    *holder = consumer_of(core, "holder");
    *registration = register_awaiting(core, *provider);
    get_accounts(core, *holder);
    CHECK(weivel_unregister(core, *registration, NULL) == WEIVEL_STATUS_OK);
    forget(core);
}

static void releasing_the_holder_ends_its_holding(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle provider, holder, registration;
    unregister_with_one_holder(core, &provider, &holder, &registration);
    CHECK(weivel_release_plugin(core, holder, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(releases_to(provider, registration) == 1);
    CHECK(revocations_to(holder, registration) == 0);
    CHECK(census_holdings(core) == 0);
    CHECK(reservations_balance(core));
    weivel_core_destroy(core);
}

static void quarantining_the_holder_ends_its_holding(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle provider, holder, registration;
    unregister_with_one_holder(core, &provider, &holder, &registration);
    CHECK(weivel_quarantine_plugin(core, holder, NULL, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(releases_to(provider, registration) == 1);
    CHECK(census_holdings(core) == 0);
    weivel_core_destroy(core);
}

static void forgetting_a_late_holder_ends_its_holding(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle provider = provider_of(core, "provider");
    weivel_handle holder = consumer_of(core, "holder");
    weivel_handle registration = register_awaiting(core, provider);
    CHECK(weivel_release_plugin(core, holder, NULL) == WEIVEL_STATUS_OK);
    get_accounts(core, holder);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(holder, registration) == 1);
    CHECK(count_kind(WEIVEL_EVENT_HOLDINGS_RELEASED) == 0);
    forget(core);
    CHECK(weivel_forget_plugin(core, holder, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(releases_to(provider, registration) == 1);
    CHECK(census_holdings(core) == 0);
    CHECK(reservations_balance(core));
    weivel_core_destroy(core);
}

static void a_release_that_ends_nothing_is_refused_as_stale(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle provider = provider_of(core, "provider");
    weivel_handle holder = consumer_of(core, "holder");
    weivel_handle bystander = consumer_of(core, "bystander");
    weivel_handle registration = register_awaiting(core, provider);
    get_accounts(core, holder);
    refused_as_stale(core, holder, registration);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    refused_as_stale(core, bystander, registration);
    refused_as_stale(core, holder, 12345);
    refused_as_stale(core, WEIVEL_HOST, registration);
    CHECK(weivel_release_holding(core, holder, registration, NULL) == WEIVEL_STATUS_OK);
    refused_as_stale(core, holder, registration);
    CHECK(weivel_release_holding(NULL, holder, registration, NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_core_destroy(core);
}

static void a_plain_registration_frees_its_holdings_as_it_always_did(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle provider = provider_of(core, "provider");
    weivel_handle holder = consumer_of(core, "holder");
    weivel_handle registration = register_service(core, provider, "accounts");
    get_accounts(core, holder);
    forget(core);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(holder, registration) == 1);
    CHECK(the_journal.count == 1);
    CHECK(census_holdings(core) == 0);
    CHECK(reservations_balance(core));
    refused_as_stale(core, holder, registration);
    weivel_core_destroy(core);
}

static void the_host_owns_and_holds_alike(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle registration = register_awaiting(core, WEIVEL_HOST);
    get_accounts(core, WEIVEL_HOST);
    forget(core);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(WEIVEL_HOST, registration) == 1);
    CHECK(count_kind(WEIVEL_EVENT_HOLDINGS_RELEASED) == 0);
    forget(core);
    CHECK(weivel_release_holding(core, WEIVEL_HOST, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(releases_to(WEIVEL_HOST, registration) == 1);
    weivel_core_destroy(core);
}

static void a_replaced_host_registration_awaits_its_holders(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle holder = consumer_of(core, "holder");
    weivel_handle replaced = register_awaiting(core, WEIVEL_HOST);
    get_accounts(core, holder);
    weivel_handle replacement = register_service(core, WEIVEL_HOST, "accounts");
    drain(core);
    CHECK(revocations_to(holder, replaced) == 1);
    CHECK(count_kind(WEIVEL_EVENT_HOLDINGS_RELEASED) == 0);
    forget(core);
    CHECK(weivel_release_holding(core, holder, replaced, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(releases_to(WEIVEL_HOST, replaced) == 1);
    CHECK(replacement != replaced);
    CHECK(reservations_balance(core));
    weivel_core_destroy(core);
}

static void releasing_the_owner_unregisters_and_awaits_its_holders(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle provider = provider_of(core, "provider");
    weivel_handle holder = consumer_of(core, "holder");
    weivel_handle registration = register_awaiting(core, provider);
    get_accounts(core, holder);
    get_accounts(core, provider);
    forget(core);
    CHECK(weivel_release_plugin(core, provider, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(holder, registration) == 1);
    CHECK(revocations_to(provider, registration) == 1);
    CHECK(count_kind(WEIVEL_EVENT_HOLDINGS_RELEASED) == 0);
    CHECK(weivel_forget_plugin(core, provider, NULL) == WEIVEL_STATUS_OK);
    forget(core);
    CHECK(weivel_release_holding(core, holder, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(releases_to(provider, registration) == 1);
    CHECK(reservations_balance(core));
    weivel_core_destroy(core);
}

static void a_malformed_registration_changes_nothing(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle registration = 7;
    CHECK(weivel_register_service_awaiting_releases(NULL, WEIVEL_HOST, "accounts", &registration, NULL) ==
          WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_register_service_awaiting_releases(core, WEIVEL_HOST, "", &registration, NULL) ==
          WEIVEL_STATUS_MALFORMED);
    CHECK(registration == 0);
    CHECK(weivel_register_service_awaiting_releases(core, WEIVEL_HOST, "accounts", NULL, NULL) ==
          WEIVEL_STATUS_MALFORMED);
    CHECK(reservations_balance(core));
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, WEIVEL_HOST, "accounts", &got, NULL) == WEIVEL_STATUS_NONE);
    weivel_core_destroy(core);
}

static void a_refused_registration_reserves_nothing(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle provider = provider_of(core, "provider");
    weivel_handle rival = provider_of(core, "rival");
    register_awaiting(core, provider);
    weivel_handle registration = 0;
    CHECK(weivel_register_service_awaiting_releases(core, rival, "accounts", &registration, NULL) ==
          WEIVEL_STATUS_REFUSED);
    CHECK(registration == 0);
    CHECK(reservations_balance(core));
    weivel_core_destroy(core);
}

static void destroy_frees_what_still_awaits_a_release(void) {
    weivel_core *core = core_knowing(accounts, 1);
    weivel_handle provider, holder, registration;
    unregister_with_one_holder(core, &provider, &holder, &registration);
    register_awaiting(core, provider);
    get_accounts(core, holder);
    CHECK(weivel_core_destroy(core) == WEIVEL_STATUS_OK);
}

int main(void) {
    the_owner_hears_once_after_the_last_release();
    the_owner_hears_at_once_when_nothing_held_it();
    releasing_the_holder_ends_its_holding();
    quarantining_the_holder_ends_its_holding();
    forgetting_a_late_holder_ends_its_holding();
    a_release_that_ends_nothing_is_refused_as_stale();
    a_plain_registration_frees_its_holdings_as_it_always_did();
    the_host_owns_and_holds_alike();
    a_replaced_host_registration_awaits_its_holders();
    releasing_the_owner_unregisters_and_awaits_its_holders();
    a_malformed_registration_changes_nothing();
    a_refused_registration_reserves_nothing();
    destroy_frees_what_still_awaits_a_release();
    return check_summary("releases_test");
}
