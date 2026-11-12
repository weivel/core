#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <weivel/core.h>

#include "check.h"

#define MAXIMUM_DECLARATIONS 4

typedef struct declarations {
    const char *capabilities_provided[MAXIMUM_DECLARATIONS];
    const char *capabilities_consumed[MAXIMUM_DECLARATIONS];
    weivel_service_offer services_provided[MAXIMUM_DECLARATIONS];
    weivel_service_want services_consumed[MAXIMUM_DECLARATIONS];
    uint32_t capabilities_provided_count;
    uint32_t capabilities_consumed_count;
    uint32_t services_provided_count;
    uint32_t services_consumed_count;
} declarations;

static declarations nothing_declared(void) {
    declarations declared;
    memset(&declared, 0, sizeof declared);
    return declared;
}

static void provide_capability(declarations *declared, const char *id) {
    declared->capabilities_provided[declared->capabilities_provided_count++] = id;
}

static void consume_capability(declarations *declared, const char *id) {
    declared->capabilities_consumed[declared->capabilities_consumed_count++] = id;
}

static void provide_service(declarations *declared, const char *id) {
    weivel_service_offer offer = WEIVEL_INIT(weivel_service_offer);
    offer.id = id;
    declared->services_provided[declared->services_provided_count++] = offer;
}

static void consume_service(declarations *declared, const char *id, uint32_t optional, uint32_t degradable) {
    weivel_service_want want = WEIVEL_INIT(weivel_service_want);
    want.id = id;
    want.optional = optional;
    want.degradable = degradable;
    declared->services_consumed[declared->services_consumed_count++] = want;
}

static weivel_handle register_plugin(weivel_core *core, const char *import_name, const declarations *declared) {
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = import_name;
    manifest.api = 1;
    manifest.capabilities.provides.items = declared->capabilities_provided;
    manifest.capabilities.provides.count = declared->capabilities_provided_count;
    manifest.capabilities.consumes.items = declared->capabilities_consumed;
    manifest.capabilities.consumes.count = declared->capabilities_consumed_count;
    manifest.services.provides.items = declared->services_provided;
    manifest.services.provides.count = declared->services_provided_count;
    manifest.services.consumes.items = declared->services_consumed;
    manifest.services.consumes.count = declared->services_consumed_count;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static weivel_core *new_core(void) {
    weivel_core *core = NULL;
    CHECK(weivel_core_create(NULL, &core, NULL) == WEIVEL_STATUS_OK);
    return core;
}

static weivel_activation_plan plan_of(weivel_core *core, const weivel_handle *plugins, uint32_t count,
                                      uint32_t edge_source, const weivel_strings *host_provided) {
    weivel_activation_plan plan = WEIVEL_INIT(weivel_activation_plan);
    uint32_t pending = 99;
    CHECK(weivel_plan_activation(core, plugins, count, edge_source, host_provided, &plan, &pending) ==
          WEIVEL_STATUS_OK);
    CHECK(pending == 0);
    return plan;
}

static void release(weivel_activation_plan *plan) {
    CHECK(weivel_activation_plan_release(plan) == WEIVEL_STATUS_OK);
    CHECK(plan->storage == NULL);
    CHECK(plan->order == NULL && plan->order_count == 0);
    CHECK(plan->refusals == NULL && plan->refusal_count == 0);
}

static void check_order(const weivel_activation_plan *plan, const weivel_handle *expected, uint32_t count) {
    CHECK(plan->order_count == count);
    for (uint32_t index = 0; index < count && index < plan->order_count; index++) {
        CHECK(plan->order[index] == expected[index]);
    }
}

static void check_no_refusal_and_no_cycle(const weivel_activation_plan *plan) {
    CHECK(plan->refusal_count == 0);
    CHECK(plan->cycle_count == 0);
}

static int has_edge(const weivel_activation_plan *plan, weivel_handle provider, weivel_handle consumer) {
    for (uint32_t index = 0; index < plan->edge_count; index++) {
        if (plan->edges[index].provider == provider && plan->edges[index].consumer == consumer) {
            return 1;
        }
    }
    return 0;
}

static void check_unmet(const weivel_plugin_refusal *refusal, weivel_handle plugin, const char *need) {
    char detail[128];
    char fix[160];
    snprintf(detail, sizeof detail, "requires \"%s\", which nothing here provides", need);
    snprintf(fix, sizeof fix, "install something that provides \"%s\", or mark the requirement optional", need);
    CHECK(refusal->plugin == plugin);
    CHECK(refusal->refusal.code == WEIVEL_REFUSAL_UNMET_NEED);
    check_text(refusal->refusal.detail, detail, "unmet detail");
    check_text(refusal->refusal.fix, fix, "unmet fix");
}

static weivel_handle register_capability_unit(weivel_core *core, const char *id, const char *consumed) {
    declarations declared = nothing_declared();
    provide_capability(&declared, id);
    if (consumed != NULL) {
        consume_capability(&declared, consumed);
    }
    return register_plugin(core, id, &declared);
}

static void orders_a_capability_provider_before_its_consumer(void) {
    weivel_core *core = new_core();
    weivel_handle api = register_capability_unit(core, "api.http", "storage.object");
    weivel_handle storage = register_capability_unit(core, "storage.object", NULL);
    weivel_handle units[] = {api, storage};
    weivel_activation_plan plan = plan_of(core, units, 2, WEIVEL_EDGES_CAPABILITIES, NULL);
    weivel_handle expected[] = {storage, api};
    check_order(&plan, expected, 2);
    check_no_refusal_and_no_cycle(&plan);
    CHECK(plan.edge_count == 1);
    CHECK(has_edge(&plan, storage, api));
    release(&plan);
    weivel_core_destroy(core);
}

static void draws_no_edge_from_a_capability_under_service_sources(void) {
    weivel_core *core = new_core();
    weivel_handle api = register_capability_unit(core, "api.http", "storage.object");
    weivel_handle storage = register_capability_unit(core, "storage.object", NULL);
    weivel_handle units[] = {api, storage};
    weivel_activation_plan plan = plan_of(core, units, 2, WEIVEL_EDGES_SERVICES, NULL);
    weivel_handle expected[] = {api, storage};
    check_order(&plan, expected, 2);
    check_no_refusal_and_no_cycle(&plan);
    CHECK(plan.edge_count == 0);
    CHECK(plan.edges == NULL);
    release(&plan);
    weivel_core_destroy(core);
}

static void orders_a_mandatory_consumer_of_a_service_the_host_provides(void) {
    weivel_core *core = new_core();
    declarations declared = nothing_declared();
    consume_service(&declared, "clock", WEIVEL_CHOICE_NO, WEIVEL_CHOICE_DEFAULT);
    weivel_handle dashboard = register_plugin(core, "dashboard", &declared);
    const char *const host_ids[] = {"clock"};
    weivel_strings host_provided = {host_ids, 1};
    weivel_activation_plan plan = plan_of(core, &dashboard, 1, WEIVEL_EDGES_SERVICES, &host_provided);
    check_order(&plan, &dashboard, 1);
    check_no_refusal_and_no_cycle(&plan);
    CHECK(plan.edge_count == 0);
    release(&plan);
    plan = plan_of(core, &dashboard, 1, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.order_count == 0);
    CHECK(plan.refusal_count == 1);
    check_unmet(&plan.refusals[0], dashboard, "clock");
    release(&plan);
    weivel_core_destroy(core);
}

static void orders_a_service_provider_before_its_consumer(void) {
    weivel_core *core = new_core();
    declarations consumer = nothing_declared();
    consume_service(&consumer, "accounts", WEIVEL_CHOICE_NO, WEIVEL_CHOICE_DEFAULT);
    weivel_handle dashboard = register_plugin(core, "dashboard", &consumer);
    declarations provider = nothing_declared();
    provide_service(&provider, "accounts");
    weivel_handle identity = register_plugin(core, "identity", &provider);
    weivel_handle units[] = {dashboard, identity};
    weivel_activation_plan plan = plan_of(core, units, 2, WEIVEL_EDGES_SERVICES, NULL);
    weivel_handle expected[] = {identity, dashboard};
    check_order(&plan, expected, 2);
    check_no_refusal_and_no_cycle(&plan);
    CHECK(plan.edge_count == 1);
    CHECK(has_edge(&plan, identity, dashboard));
    release(&plan);
    weivel_core_destroy(core);
}

static void orders_an_optional_requirement_that_is_present(void) {
    weivel_core *core = new_core();
    weivel_handle api = register_capability_unit(core, "api.http", "storage.object");
    weivel_handle storage = register_capability_unit(core, "storage.object", NULL);
    weivel_handle units[] = {api, storage};
    weivel_activation_plan plan = plan_of(core, units, 2, WEIVEL_EDGES_CAPABILITIES, NULL);
    weivel_handle expected[] = {storage, api};
    check_order(&plan, expected, 2);
    check_no_refusal_and_no_cycle(&plan);
    CHECK(plan.edge_count == 1);
    CHECK(has_edge(&plan, storage, api));
    release(&plan);
    weivel_core_destroy(core);
}

static int cycle_contains(const weivel_cycle *cycle, weivel_handle member) {
    for (uint32_t index = 0; index < cycle->count; index++) {
        if (cycle->members[index] == member) {
            return 1;
        }
    }
    return 0;
}

static void reports_a_cycle_with_its_members(void) {
    weivel_core *core = new_core();
    weivel_handle api = register_capability_unit(core, "api.http", "storage.object");
    weivel_handle storage = register_capability_unit(core, "storage.object", "api.http");
    weivel_handle units[] = {api, storage};
    weivel_activation_plan plan = plan_of(core, units, 2, WEIVEL_EDGES_CAPABILITIES, NULL);
    CHECK(plan.order_count == 0);
    CHECK(plan.order == NULL);
    CHECK(plan.refusal_count == 0);
    CHECK(plan.cycle_count == 1);
    if (plan.cycle_count == 1) {
        CHECK(plan.cycles[0].count == 2);
        CHECK(cycle_contains(&plan.cycles[0], api));
        CHECK(cycle_contains(&plan.cycles[0], storage));
    }
    CHECK(plan.edge_count == 2);
    CHECK(has_edge(&plan, storage, api));
    CHECK(has_edge(&plan, api, storage));
    release(&plan);
    weivel_core_destroy(core);
}

static weivel_handle register_boot_unit(weivel_core *core, uint32_t optional, uint32_t degradable) {
    declarations declared = nothing_declared();
    provide_service(&declared, "api.http");
    consume_service(&declared, "storage.object", optional, degradable);
    return register_plugin(core, "api.http", &declared);
}

static void refuses_a_degradable_requirement_that_is_not_optional(void) {
    weivel_core *core = new_core();
    weivel_handle api = register_boot_unit(core, WEIVEL_CHOICE_NO, WEIVEL_CHOICE_YES);
    weivel_activation_plan plan = plan_of(core, &api, 1, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.order_count == 0);
    CHECK(plan.refusal_count == 1);
    check_unmet(&plan.refusals[0], api, "storage.object");
    CHECK(plan.cycle_count == 0);
    CHECK(plan.edge_count == 0);
    release(&plan);
    weivel_core_destroy(core);
}

static void refuses_a_missing_mandatory_requirement(void) {
    weivel_core *core = new_core();
    weivel_handle api = register_boot_unit(core, WEIVEL_CHOICE_NO, WEIVEL_CHOICE_NO);
    weivel_activation_plan plan = plan_of(core, &api, 1, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.order_count == 0);
    CHECK(plan.refusal_count == 1);
    check_unmet(&plan.refusals[0], api, "storage.object");
    CHECK(plan.cycle_count == 0);
    CHECK(plan.edge_count == 0);
    release(&plan);
    weivel_core_destroy(core);
}

static void starts_without_a_missing_optional_requirement(void) {
    weivel_core *core = new_core();
    weivel_handle api = register_boot_unit(core, WEIVEL_CHOICE_YES, WEIVEL_CHOICE_NO);
    weivel_activation_plan plan = plan_of(core, &api, 1, WEIVEL_EDGES_SERVICES, NULL);
    check_order(&plan, &api, 1);
    check_no_refusal_and_no_cycle(&plan);
    CHECK(plan.edge_count == 0);
    release(&plan);
    weivel_core_destroy(core);
}

static void an_absent_optional_choice_waits_for_nothing(void) {
    weivel_core *core = new_core();
    weivel_handle api = register_boot_unit(core, WEIVEL_CHOICE_DEFAULT, WEIVEL_CHOICE_DEFAULT);
    weivel_activation_plan plan = plan_of(core, &api, 1, WEIVEL_EDGES_SERVICES, NULL);
    check_order(&plan, &api, 1);
    check_no_refusal_and_no_cycle(&plan);
    release(&plan);
    weivel_core_destroy(core);
}

static void a_consumed_capability_is_always_optional(void) {
    weivel_core *core = new_core();
    weivel_handle api = register_capability_unit(core, "api.http", "storage.object");
    weivel_activation_plan plan = plan_of(core, &api, 1, WEIVEL_EDGES_CAPABILITIES, NULL);
    check_order(&plan, &api, 1);
    check_no_refusal_and_no_cycle(&plan);
    release(&plan);
    weivel_core_destroy(core);
}

static weivel_handle register_service_unit(weivel_core *core, const char *name, const char *provided,
                                           const char *mandatory) {
    declarations declared = nothing_declared();
    if (provided != NULL) {
        provide_service(&declared, provided);
    }
    if (mandatory != NULL) {
        consume_service(&declared, mandatory, WEIVEL_CHOICE_NO, WEIVEL_CHOICE_NO);
    }
    return register_plugin(core, name, &declared);
}

static void refusal_runs_to_a_fixpoint(void) {
    weivel_core *core = new_core();
    weivel_handle top = register_service_unit(core, "top", NULL, "middle");
    weivel_handle middle = register_service_unit(core, "middle", "middle", "bottom");
    weivel_handle free_standing = register_service_unit(core, "free", NULL, NULL);
    weivel_handle units[] = {top, middle, free_standing};
    weivel_activation_plan plan = plan_of(core, units, 3, WEIVEL_EDGES_SERVICES, NULL);
    check_order(&plan, &free_standing, 1);
    CHECK(plan.refusal_count == 2);
    if (plan.refusal_count == 2) {
        check_unmet(&plan.refusals[0], middle, "bottom");
        check_unmet(&plan.refusals[1], top, "middle");
    }
    CHECK(plan.edge_count == 0);
    release(&plan);
    weivel_core_destroy(core);
}

static void a_cycle_member_provides_nothing(void) {
    weivel_core *core = new_core();
    weivel_handle first = register_service_unit(core, "first", "first", "second");
    weivel_handle second = register_service_unit(core, "second", "second", "first");
    weivel_handle mandatory = register_service_unit(core, "mandatory", NULL, "first");
    declarations declared = nothing_declared();
    consume_service(&declared, "second", WEIVEL_CHOICE_YES, WEIVEL_CHOICE_NO);
    weivel_handle optional = register_plugin(core, "optional", &declared);
    weivel_handle units[] = {first, second, mandatory, optional};
    weivel_activation_plan plan = plan_of(core, units, 4, WEIVEL_EDGES_SERVICES, NULL);
    check_order(&plan, &optional, 1);
    CHECK(plan.cycle_count == 1);
    CHECK(plan.refusal_count == 1);
    if (plan.refusal_count == 1) {
        check_unmet(&plan.refusals[0], mandatory, "first");
    }
    CHECK(has_edge(&plan, second, optional));
    CHECK(!has_edge(&plan, first, mandatory));
    release(&plan);
    weivel_core_destroy(core);
}

static void independent_plugins_are_placed_in_sweeps_of_the_input(void) {
    weivel_core *core = new_core();
    weivel_handle consumer = register_service_unit(core, "consumer", NULL, "provider");
    weivel_handle provider = register_service_unit(core, "provider", "provider", NULL);
    weivel_handle bystander = register_service_unit(core, "bystander", NULL, NULL);
    weivel_handle late_first[] = {consumer, provider, bystander};
    weivel_activation_plan plan = plan_of(core, late_first, 3, WEIVEL_EDGES_SERVICES, NULL);
    weivel_handle late_expected[] = {provider, bystander, consumer};
    check_order(&plan, late_expected, 3);
    release(&plan);
    weivel_handle provider_first[] = {provider, consumer, bystander};
    plan = plan_of(core, provider_first, 3, WEIVEL_EDGES_SERVICES, NULL);
    weivel_handle provider_expected[] = {provider, consumer, bystander};
    check_order(&plan, provider_expected, 3);
    release(&plan);
    weivel_core_destroy(core);
}

static void the_hosts_services_count_as_placed_after_the_first_sweep(void) {
    weivel_core *core = new_core();
    weivel_handle dashboard = register_service_unit(core, "dashboard", NULL, "clock");
    weivel_handle other = register_service_unit(core, "other", NULL, NULL);
    const char *const host_ids[] = {"clock"};
    weivel_strings host_provided = {host_ids, 1};
    weivel_handle units[] = {dashboard, other};
    weivel_activation_plan plan = plan_of(core, units, 2, WEIVEL_EDGES_SERVICES, &host_provided);
    weivel_handle expected[] = {other, dashboard};
    check_order(&plan, expected, 2);
    check_no_refusal_and_no_cycle(&plan);
    CHECK(plan.edge_count == 0);
    release(&plan);
    weivel_core_destroy(core);
}

static void a_cycles_members_read_as_a_chain_from_where_the_search_entered(void) {
    weivel_core *core = new_core();
    weivel_handle a = register_service_unit(core, "a", "a", "b");
    weivel_handle b = register_service_unit(core, "b", "b", "c");
    weivel_handle c = register_service_unit(core, "c", "c", "a");
    weivel_handle units[] = {a, b, c};
    weivel_activation_plan plan = plan_of(core, units, 3, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.order_count == 0);
    CHECK(plan.cycle_count == 1);
    if (plan.cycle_count == 1 && plan.cycles[0].count == 3) {
        CHECK(plan.cycles[0].members[0] == a && plan.cycles[0].members[1] == b && plan.cycles[0].members[2] == c);
    } else {
        CHECK(0);
    }
    release(&plan);
    weivel_core_destroy(core);
}

static void a_mandatory_service_need_refuses_nothing_under_capability_sources(void) {
    weivel_core *core = new_core();
    declarations declared = nothing_declared();
    provide_capability(&declared, "api.http");
    consume_service(&declared, "missing", WEIVEL_CHOICE_NO, WEIVEL_CHOICE_NO);
    weivel_handle api = register_plugin(core, "api.http", &declared);
    weivel_activation_plan plan = plan_of(core, &api, 1, WEIVEL_EDGES_CAPABILITIES, NULL);
    check_order(&plan, &api, 1);
    check_no_refusal_and_no_cycle(&plan);
    release(&plan);
    weivel_core_destroy(core);
}

static void a_host_id_matching_a_capability_is_ignored_under_capability_sources(void) {
    weivel_core *core = new_core();
    declarations declared = nothing_declared();
    consume_capability(&declared, "clock");
    weivel_handle dashboard = register_plugin(core, "dashboard", &declared);
    weivel_handle other = register_service_unit(core, "other", NULL, NULL);
    const char *const host_ids[] = {"clock"};
    weivel_strings host_provided = {host_ids, 1};
    weivel_handle units[] = {dashboard, other};
    weivel_activation_plan plan = plan_of(core, units, 2, WEIVEL_EDGES_CAPABILITIES, &host_provided);
    weivel_handle expected[] = {dashboard, other};
    check_order(&plan, expected, 2);
    check_no_refusal_and_no_cycle(&plan);
    CHECK(plan.edge_count == 0);
    release(&plan);
    weivel_core_destroy(core);
}

static void check_cycle(const weivel_activation_plan *plan, uint32_t cycle, const weivel_handle *members,
                        uint32_t count) {
    CHECK(cycle < plan->cycle_count && plan->cycles[cycle].count == count);
    for (uint32_t index = 0; cycle < plan->cycle_count && index < count && index < plan->cycles[cycle].count;
         index++) {
        CHECK(plan->cycles[cycle].members[index] == members[index]);
    }
}

static void disjoint_cycles_come_back_in_the_order_the_search_meets_them(void) {
    weivel_core *core = new_core();
    weivel_handle a = register_service_unit(core, "a", "a", "b");
    weivel_handle b = register_service_unit(core, "b", "b", "a");
    weivel_handle c = register_service_unit(core, "c", "c", "d");
    weivel_handle d = register_service_unit(core, "d", "d", "c");
    weivel_handle first_pair[] = {a, b};
    weivel_handle second_pair[] = {c, d};
    weivel_handle units[] = {a, b, c, d};
    weivel_activation_plan plan = plan_of(core, units, 4, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.order_count == 0 && plan.refusal_count == 0 && plan.cycle_count == 2);
    check_cycle(&plan, 0, first_pair, 2);
    check_cycle(&plan, 1, second_pair, 2);
    release(&plan);
    weivel_handle swapped[] = {c, d, a, b};
    plan = plan_of(core, swapped, 4, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.cycle_count == 2);
    check_cycle(&plan, 0, second_pair, 2);
    check_cycle(&plan, 1, first_pair, 2);
    release(&plan);
    weivel_core_destroy(core);
}

static void a_figure_eight_of_cycles_is_one_component(void) {
    weivel_core *core = new_core();
    weivel_handle a = register_service_unit(core, "a", "a", "b");
    declarations declared = nothing_declared();
    provide_service(&declared, "b");
    consume_service(&declared, "a", WEIVEL_CHOICE_NO, WEIVEL_CHOICE_NO);
    consume_service(&declared, "c", WEIVEL_CHOICE_NO, WEIVEL_CHOICE_NO);
    weivel_handle b = register_plugin(core, "b", &declared);
    weivel_handle c = register_service_unit(core, "c", "c", "b");
    weivel_handle units[] = {a, b, c};
    weivel_activation_plan plan = plan_of(core, units, 3, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.order_count == 0 && plan.refusal_count == 0 && plan.cycle_count == 1);
    weivel_handle members[] = {a, b, c};
    check_cycle(&plan, 0, members, 3);
    release(&plan);
    weivel_core_destroy(core);
}

static void a_provider_meeting_two_needs_draws_one_edge(void) {
    weivel_core *core = new_core();
    declarations provider = nothing_declared();
    provide_service(&provider, "first");
    provide_service(&provider, "second");
    weivel_handle both = register_plugin(core, "both", &provider);
    declarations consumer = nothing_declared();
    consume_service(&consumer, "first", WEIVEL_CHOICE_NO, WEIVEL_CHOICE_NO);
    consume_service(&consumer, "second", WEIVEL_CHOICE_NO, WEIVEL_CHOICE_NO);
    weivel_handle needs_both = register_plugin(core, "needs_both", &consumer);
    weivel_handle units[] = {needs_both, both};
    weivel_activation_plan plan = plan_of(core, units, 2, WEIVEL_EDGES_SERVICES, NULL);
    weivel_handle expected[] = {both, needs_both};
    check_order(&plan, expected, 2);
    CHECK(plan.edge_count == 1);
    CHECK(has_edge(&plan, both, needs_both));
    release(&plan);
    weivel_core_destroy(core);
}

static void a_plugin_meets_its_own_need_without_an_edge(void) {
    weivel_core *core = new_core();
    weivel_handle itself = register_service_unit(core, "itself", "loop", "loop");
    weivel_activation_plan plan = plan_of(core, &itself, 1, WEIVEL_EDGES_SERVICES, NULL);
    check_order(&plan, &itself, 1);
    check_no_refusal_and_no_cycle(&plan);
    CHECK(plan.edge_count == 0);
    release(&plan);
    weivel_core_destroy(core);
}

static void two_providers_both_come_first(void) {
    weivel_core *core = new_core();
    weivel_handle consumer = register_service_unit(core, "consumer", NULL, "shared");
    weivel_handle one = register_service_unit(core, "one", "shared", NULL);
    weivel_handle two = register_service_unit(core, "two", "shared", NULL);
    weivel_handle units[] = {consumer, one, two};
    weivel_activation_plan plan = plan_of(core, units, 3, WEIVEL_EDGES_SERVICES, NULL);
    weivel_handle expected[] = {one, two, consumer};
    check_order(&plan, expected, 3);
    CHECK(plan.edge_count == 2);
    if (plan.edge_count == 2) {
        CHECK(plan.edges[0].provider == one && plan.edges[0].consumer == consumer);
        CHECK(plan.edges[1].provider == two && plan.edges[1].consumer == consumer);
    }
    release(&plan);
    weivel_core_destroy(core);
}

static void a_stale_or_repeated_handle_is_refused_as_data(void) {
    weivel_core *core = new_core();
    weivel_handle provider = register_service_unit(core, "provider", "provider", NULL);
    weivel_handle consumer = register_service_unit(core, "consumer", NULL, "provider");
    weivel_handle stale = provider + ((weivel_handle)1 << 32);
    weivel_handle units[] = {stale, consumer, 0, provider, consumer};
    weivel_activation_plan plan = plan_of(core, units, 5, WEIVEL_EDGES_SERVICES, NULL);
    weivel_handle expected[] = {provider, consumer};
    check_order(&plan, expected, 2);
    CHECK(plan.refusal_count == 3);
    if (plan.refusal_count == 3) {
        CHECK(plan.refusals[0].plugin == stale);
        CHECK(plan.refusals[0].refusal.code == WEIVEL_REFUSAL_STALE_HANDLE);
        check_text(plan.refusals[0].refusal.fix,
                   "pass only handles weivel_register_manifest answered on this core", "stale fix");
        CHECK(plan.refusals[1].plugin == 0);
        CHECK(plan.refusals[1].refusal.code == WEIVEL_REFUSAL_STALE_HANDLE);
        CHECK(plan.refusals[2].plugin == consumer);
        CHECK(plan.refusals[2].refusal.code == WEIVEL_REFUSAL_REPEATED_PLUGIN);
        check_text(plan.refusals[2].refusal.detail, "the plugin \"consumer\" is listed more than once",
                   "repeated detail");
        check_text(plan.refusals[2].refusal.fix, "list each plugin once", "repeated fix");
    }
    CHECK(plan.edge_count == 1);
    release(&plan);
    weivel_core_destroy(core);
}

static void an_empty_set_plans_nothing(void) {
    weivel_core *core = new_core();
    weivel_activation_plan plan = plan_of(core, NULL, 0, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.order_count == 0 && plan.edge_count == 0 && plan.cycle_count == 0 && plan.refusal_count == 0);
    CHECK(plan.order == NULL && plan.edges == NULL && plan.cycles == NULL && plan.refusals == NULL);
    release(&plan);
    weivel_core_destroy(core);
}

static void a_long_chain_is_ordered_and_a_long_cycle_reported(void) {
    enum { LENGTH = 3000 };
    weivel_core *core = new_core();
    static char names[LENGTH][16];
    weivel_handle *units = calloc(LENGTH, sizeof *units);
    CHECK(units != NULL);
    if (units == NULL) {
        return;
    }
    for (uint32_t index = 0; index < LENGTH; index++) {
        snprintf(names[index], sizeof names[index], "link%u", index);
    }
    for (uint32_t index = 0; index < LENGTH; index++) {
        units[index] = register_service_unit(core, names[index], names[index],
                                             index + 1 < LENGTH ? names[index + 1] : NULL);
    }
    weivel_activation_plan plan = plan_of(core, units, LENGTH, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.order_count == LENGTH);
    CHECK(plan.order_count == LENGTH && plan.order[0] == units[LENGTH - 1] && plan.order[LENGTH - 1] == units[0]);
    release(&plan);
    weivel_core_destroy(core);

    core = new_core();
    for (uint32_t index = 0; index < LENGTH; index++) {
        units[index] = register_service_unit(core, names[index], names[index], names[(index + 1) % LENGTH]);
    }
    plan = plan_of(core, units, LENGTH, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.order_count == 0);
    CHECK(plan.refusal_count == 0);
    CHECK(plan.cycle_count == 1 && plan.cycles[0].count == LENGTH);
    release(&plan);
    weivel_core_destroy(core);
    free(units);
}

static void a_cascading_refusal_chain_of_2000_completes(void) {
    enum { LENGTH = 2000 };
    weivel_core *core = new_core();
    static char names[LENGTH + 1][16];
    static weivel_handle units[LENGTH];
    for (uint32_t index = 0; index <= LENGTH; index++) {
        snprintf(names[index], sizeof names[index], "link%u", index);
    }
    for (uint32_t index = 0; index < LENGTH; index++) {
        units[index] = register_service_unit(core, names[index], names[index], names[index + 1]);
    }
    weivel_activation_plan plan = plan_of(core, units, LENGTH, WEIVEL_EDGES_SERVICES, NULL);
    CHECK(plan.order_count == 0 && plan.cycle_count == 0 && plan.edge_count == 0);
    CHECK(plan.refusal_count == LENGTH);
    if (plan.refusal_count == LENGTH) {
        check_unmet(&plan.refusals[0], units[LENGTH - 1], names[LENGTH]);
        check_unmet(&plan.refusals[LENGTH - 1], units[0], names[1]);
    }
    release(&plan);
    weivel_core_destroy(core);
}

static weivel_event drain(weivel_core *core) {
    weivel_event event = WEIVEL_INIT(weivel_event);
    CHECK(weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK);
    CHECK(event.kind == WEIVEL_EVENT_REFUSAL);
    CHECK(event.data.refusal.code == WEIVEL_REFUSAL_MALFORMED_CALL);
    return event;
}

static void check_malformed(weivel_core *core, const weivel_handle *plugins, uint32_t count, uint32_t edge_source,
                            const weivel_strings *host_provided, weivel_activation_plan *plan) {
    uint32_t pending = 0;
    CHECK(weivel_plan_activation(core, plugins, count, edge_source, host_provided, plan, &pending) ==
          WEIVEL_STATUS_MALFORMED);
    CHECK(pending == 1);
    weivel_event event = drain(core);
    weivel_event_release(&event);
}

static void malformed_plan_calls_are_answered_not_crashed(void) {
    weivel_core *core = new_core();
    weivel_handle plugin = register_service_unit(core, "plugin", NULL, NULL);
    weivel_activation_plan plan = WEIVEL_INIT(weivel_activation_plan);
    CHECK(weivel_plan_activation(NULL, &plugin, 1, WEIVEL_EDGES_SERVICES, NULL, &plan, NULL) ==
          WEIVEL_STATUS_MALFORMED);
    check_malformed(core, &plugin, 1, WEIVEL_EDGES_SERVICES, NULL, NULL);
    weivel_activation_plan small = WEIVEL_INIT(weivel_activation_plan);
    small.size = 8;
    check_malformed(core, &plugin, 1, WEIVEL_EDGES_SERVICES, NULL, &small);
    check_malformed(core, NULL, 1, WEIVEL_EDGES_SERVICES, NULL, &plan);
    check_malformed(core, &plugin, 1, 0, NULL, &plan);
    check_malformed(core, &plugin, 1, 3, NULL, &plan);
    weivel_strings pointing_at_none = {NULL, 1};
    check_malformed(core, &plugin, 1, WEIVEL_EDGES_SERVICES, &pointing_at_none, &plan);
    const char *const holding_null[] = {"clock", NULL};
    weivel_strings with_null = {holding_null, 2};
    check_malformed(core, &plugin, 1, WEIVEL_EDGES_SERVICES, &with_null, &plan);
    CHECK(plan.storage == NULL);

    plan = plan_of(core, &plugin, 1, WEIVEL_EDGES_SERVICES, NULL);
    const weivel_handle *held = plan.order;
    check_malformed(core, &plugin, 1, WEIVEL_EDGES_SERVICES, NULL, &plan);
    CHECK(plan.order == held && plan.order_count == 1 && plan.order[0] == plugin);
    release(&plan);
    weivel_core_destroy(core);
}

static void release_is_a_no_op_on_nothing_held(void) {
    weivel_activation_plan empty = WEIVEL_INIT(weivel_activation_plan);
    CHECK(weivel_activation_plan_release(&empty) == WEIVEL_STATUS_OK);
    weivel_activation_plan zero;
    memset(&zero, 0, sizeof zero);
    CHECK(weivel_activation_plan_release(&zero) == WEIVEL_STATUS_OK);
    CHECK(weivel_activation_plan_release(NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_activation_plan small = WEIVEL_INIT(weivel_activation_plan);
    small.size = 8;
    CHECK(weivel_activation_plan_release(&small) == WEIVEL_STATUS_MALFORMED);
}

typedef struct newer_plan {
    weivel_activation_plan plan;
    uint64_t appended[2];
} newer_plan;

static void a_newer_callers_larger_plan_has_its_unknown_fields_zeroed(void) {
    weivel_core *core = new_core();
    weivel_handle plugin = register_service_unit(core, "plugin", NULL, NULL);
    newer_plan newer;
    memset(&newer, 0xee, sizeof newer);
    newer.plan.size = (uint32_t)sizeof newer;
    newer.plan.storage = NULL;
    CHECK(weivel_plan_activation(core, &plugin, 1, WEIVEL_EDGES_SERVICES, NULL, &newer.plan, NULL) ==
          WEIVEL_STATUS_OK);
    CHECK(newer.plan.size == sizeof newer);
    CHECK(newer.plan.order_count == 1 && newer.plan.order[0] == plugin);
    CHECK(newer.appended[0] == 0 && newer.appended[1] == 0);
    CHECK(weivel_activation_plan_release(&newer.plan) == WEIVEL_STATUS_OK);
    CHECK(newer.plan.size == sizeof newer);
    weivel_core_destroy(core);
}

int main(void) {
    orders_a_capability_provider_before_its_consumer();
    draws_no_edge_from_a_capability_under_service_sources();
    orders_a_mandatory_consumer_of_a_service_the_host_provides();
    orders_a_service_provider_before_its_consumer();
    orders_an_optional_requirement_that_is_present();
    reports_a_cycle_with_its_members();
    refuses_a_degradable_requirement_that_is_not_optional();
    refuses_a_missing_mandatory_requirement();
    starts_without_a_missing_optional_requirement();
    an_absent_optional_choice_waits_for_nothing();
    a_consumed_capability_is_always_optional();
    refusal_runs_to_a_fixpoint();
    a_cycle_member_provides_nothing();
    independent_plugins_are_placed_in_sweeps_of_the_input();
    the_hosts_services_count_as_placed_after_the_first_sweep();
    a_cycles_members_read_as_a_chain_from_where_the_search_entered();
    a_mandatory_service_need_refuses_nothing_under_capability_sources();
    a_host_id_matching_a_capability_is_ignored_under_capability_sources();
    disjoint_cycles_come_back_in_the_order_the_search_meets_them();
    a_figure_eight_of_cycles_is_one_component();
    a_provider_meeting_two_needs_draws_one_edge();
    a_plugin_meets_its_own_need_without_an_edge();
    two_providers_both_come_first();
    a_stale_or_repeated_handle_is_refused_as_data();
    an_empty_set_plans_nothing();
    a_long_chain_is_ordered_and_a_long_cycle_reported();
    a_cascading_refusal_chain_of_2000_completes();
    malformed_plan_calls_are_answered_not_crashed();
    release_is_a_no_op_on_nothing_held();
    a_newer_callers_larger_plan_has_its_unknown_fields_zeroed();
    return check_summary("activation_test");
}
