#include <stddef.h>
#include <string.h>
#include <weivel/core.h>

#include "registry_support.h"

#define GUARD 0xab

static const char *const no_well_known[] = {"shared"};

static weivel_core *fresh_core(void) {
    return core_knowing(no_well_known, 1);
}

static void check_named(const seen *item, const char *subject, uint32_t has_fix, const char *label) {
    CHECK(item->named == (subject != NULL ? 1u : 0u));
    if (subject != NULL) {
        check_text(item->about, subject, label);
    }
    CHECK(item->has_fix == has_fix);
}

static const seen *latest_refusal(void) {
    return nth_of(WEIVEL_EVENT_REFUSAL, count_kind(WEIVEL_EVENT_REFUSAL) - 1);
}

static void a_want_for_an_undeclared_id_names_the_id(void) {
    weivel_core *core = fresh_core();
    plugin_spec spec = {.identity = "asker"};
    weivel_handle plugin = open_spec(core, &spec);
    forget(core);
    weivel_handle want = want_untimed(core, plugin, "elsewhere:thing");
    drain(core);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->code == WEIVEL_REFUSAL_UNDECLARED);
    if (settled != NULL) {
        check_named(settled, "elsewhere:thing", 1, "undeclared want subject");
    }
    check_named(nth_of(WEIVEL_EVENT_REACH_REFUSED, 0), "elsewhere:thing", 1, "reach subject");
    weivel_core_destroy(core);
}

static void a_timed_out_and_a_stopped_want_name_the_service(void) {
    weivel_core *core = fresh_core();
    plugin_spec spec = {.identity = "asker", .consumes = {"shared", "other:thing"}};
    weivel_handle plugin = open_spec(core, &spec);
    forget(core);
    weivel_handle timed = want_within(core, plugin, "shared", 0, 10);
    weivel_handle stopped = want_untimed(core, plugin, "other:thing");
    CHECK(weivel_advance(core, 10, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    const seen *timed_out = settlement_of(timed);
    const seen *stopped_want = settlement_of(stopped);
    CHECK(timed_out != NULL && timed_out->code == WEIVEL_REFUSAL_TIMED_OUT);
    CHECK(stopped_want != NULL && stopped_want->code == WEIVEL_REFUSAL_STOPPED);
    if (timed_out != NULL && stopped_want != NULL) {
        check_named(timed_out, "shared", 1, "timed out subject");
        check_named(stopped_want, "other:thing", 1, "stopped subject");
    }
    weivel_core_destroy(core);
}

static void a_served_want_names_nothing(void) {
    weivel_core *core = fresh_core();
    plugin_spec spec = {.identity = "asker", .consumes = {"shared"}};
    weivel_handle plugin = open_spec(core, &spec);
    register_service(core, WEIVEL_HOST, "shared");
    forget(core);
    weivel_handle want = want_untimed(core, plugin, "shared");
    drain(core);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->implementation != 0);
    if (settled != NULL) {
        check_named(settled, NULL, 0, "served want");
    }
    weivel_core_destroy(core);
}

static weivel_status register_named(weivel_core *core, const char *import_name, const char *identity,
                                    weivel_handle *plugin) {
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = import_name;
    manifest.identity = identity;
    manifest.api = 1;
    return weivel_register_manifest(core, &manifest, plugin, NULL);
}

static void a_taken_name_identity_and_state_name_the_plugin(void) {
    weivel_core *core = fresh_core();
    weivel_handle first = 0;
    weivel_handle second = 0;
    CHECK(register_named(core, "first", "first-identity", &first) == WEIVEL_STATUS_OK);
    forget(core);
    CHECK(register_named(core, "first", NULL, &second) == WEIVEL_STATUS_REFUSED);
    drain(core);
    check_named(latest_refusal(), "first", 1, "name taken");
    CHECK(register_named(core, "second", "first-identity", &second) == WEIVEL_STATUS_REFUSED);
    drain(core);
    check_named(latest_refusal(), "first-identity", 1, "identity taken");
    CHECK(weivel_open_plugin(core, first, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, first, NULL, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(latest_refusal()->code == WEIVEL_REFUSAL_PLUGIN_STATE);
    check_named(latest_refusal(), "first", 1, "plugin state");
    weivel_core_destroy(core);
}

static void a_malformed_manifest_names_its_import_name(void) {
    weivel_core *core = fresh_core();
    const char *const targets[] = {"app", NULL};
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "broken";
    manifest.api = 1;
    manifest.targets.items = targets;
    manifest.targets.count = 2;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(latest_refusal()->code == WEIVEL_REFUSAL_MALFORMED_MANIFEST);
    check_named(latest_refusal(), "broken", 1, "malformed manifest");
    weivel_core_destroy(core);
}

static void a_malformed_call_and_a_stale_handle_name_nothing(void) {
    weivel_core *core = fresh_core();
    CHECK(weivel_next_deadline(core, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    drain(core);
    CHECK(latest_refusal()->code == WEIVEL_REFUSAL_MALFORMED_CALL);
    check_named(latest_refusal(), NULL, 1, "malformed call");
    CHECK(weivel_unregister(core, 77, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(latest_refusal()->code == WEIVEL_REFUSAL_STALE_HANDLE);
    check_named(latest_refusal(), NULL, 1, "stale handle");
    weivel_core_destroy(core);
}

static void a_refused_registration_names_the_service(void) {
    weivel_core *core = fresh_core();
    plugin_spec owner = {.identity = "owner"};
    plugin_spec rival = {.identity = "rival"};
    weivel_handle first = open_spec(core, &owner);
    weivel_handle second = open_spec(core, &rival);
    register_service(core, first, "shared");
    forget(core);
    weivel_handle registration = 0;
    CHECK(weivel_register_service(core, second, "owner:thing", &registration, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(latest_refusal()->code == WEIVEL_REFUSAL_OUTSIDE_NAMESPACE);
    check_named(latest_refusal(), "owner:thing", 1, "outside namespace");
    CHECK(weivel_register_service(core, second, "shared", &registration, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(latest_refusal()->code == WEIVEL_REFUSAL_SERVICE_TAKEN);
    check_named(latest_refusal(), "shared", 1, "service taken");
    weivel_core_destroy(core);
}

static weivel_handle open_provider_of(weivel_core *core, const char *import_name, const char *capability) {
    const char *const provides[] = {capability};
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = import_name;
    manifest.api = 1;
    manifest.capabilities.provides.items = provides;
    manifest.capabilities.provides.count = 1;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static weivel_ledger_row row_of(weivel_core *core, weivel_handle plugin) {
    weivel_ledger_row row = WEIVEL_INIT(weivel_ledger_row);
    CHECK(weivel_ledger_entry(core, plugin, &row, NULL) == WEIVEL_STATUS_OK);
    return row;
}

static void a_refused_activation_names_the_import_name_in_its_event_and_its_row(void) {
    weivel_core *core = fresh_core();
    weivel_handle plugin = open_provider_of(core, "maker", "render");
    weivel_handle implementation = 0;
    CHECK(weivel_provide_capability(core, plugin, "render", &implementation, NULL) == WEIVEL_STATUS_OK);
    forget(core);
    CHECK(weivel_provide_capability(core, plugin, "render", &implementation, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(latest_refusal()->code == WEIVEL_REFUSAL_PROVIDED_TWICE);
    check_named(latest_refusal(), "render", 1, "provided twice");

    weivel_handle idle = open_provider_of(core, "idle", "paint");
    forget(core);
    CHECK(weivel_verify_activation(core, idle, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(latest_refusal()->code == WEIVEL_REFUSAL_ACTIVATION_MISMATCH);
    check_named(latest_refusal(), "idle", 1, "activation mismatch");
    CHECK(weivel_quarantine_plugin(core, idle, NULL, NULL) == WEIVEL_STATUS_OK);
    weivel_ledger_row row = row_of(core, idle);
    CHECK(row.refusal.code == WEIVEL_REFUSAL_ACTIVATION_MISMATCH);
    check_text(row.subject, "idle", "row subject");
    CHECK(row.has_fix == 1);
    CHECK(weivel_ledger_row_release(&row) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

static void a_quarantine_reason_names_nothing_and_has_a_fix_only_when_given_one(void) {
    weivel_core *core = fresh_core();
    plugin_spec spec = {.identity = "thrower"};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_quarantine_reason reason = WEIVEL_INIT(weivel_quarantine_reason);
    reason.detail = "activate threw";
    reason.fix = "";
    CHECK(weivel_quarantine_plugin(core, plugin, &reason, NULL) == WEIVEL_STATUS_OK);
    weivel_ledger_row row = row_of(core, plugin);
    CHECK(row.subject == NULL);
    CHECK(row.has_fix == 0);
    CHECK(weivel_ledger_row_release(&row) == WEIVEL_STATUS_OK);
    reason.fix = "stop throwing";
    CHECK(weivel_quarantine_plugin(core, plugin, &reason, NULL) == WEIVEL_STATUS_OK);
    row = row_of(core, plugin);
    CHECK(row.subject == NULL);
    CHECK(row.has_fix == 1);
    CHECK(weivel_ledger_row_release(&row) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

static void fill_guard(void *start, size_t length) {
    memset(start, GUARD, length);
}

static int guard_intact(const void *start, size_t length) {
    const unsigned char *bytes = start;
    for (size_t index = 0; index < length; index++) {
        if (bytes[index] != GUARD) {
            return 0;
        }
    }
    return 1;
}

static void an_old_sized_row_reads_none_of_the_subject(void) {
    weivel_core *core = fresh_core();
    weivel_handle idle = open_provider_of(core, "idle", "paint");
    CHECK(weivel_verify_activation(core, idle, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_quarantine_plugin(core, idle, NULL, NULL) == WEIVEL_STATUS_OK);
    weivel_ledger_row row = WEIVEL_INIT(weivel_ledger_row);
    size_t first_shape = offsetof(weivel_ledger_row, subject);
    fill_guard((unsigned char *)&row + first_shape, sizeof row - first_shape);
    row.size = (uint32_t)first_shape;
    CHECK(weivel_ledger_entry(core, idle, &row, NULL) == WEIVEL_STATUS_OK);
    CHECK(row.refusal.code == WEIVEL_REFUSAL_ACTIVATION_MISMATCH);
    CHECK(guard_intact((unsigned char *)&row + first_shape, sizeof row - first_shape));
    CHECK(weivel_ledger_row_release(&row) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

static void a_ledger_refusal_names_the_id_it_reached_for(void) {
    weivel_core *core = fresh_core();
    plugin_spec spec = {.identity = "asker"};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_handle implementation = 0;
    CHECK(weivel_get_service(core, plugin, "elsewhere:thing", &implementation, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_ledger_snapshot snapshot = WEIVEL_INIT(weivel_ledger_snapshot);
    CHECK(weivel_ledger(core, &snapshot, NULL) == WEIVEL_STATUS_OK);
    CHECK(snapshot.refusal_count == 1);
    if (snapshot.refusal_count == 1) {
        CHECK(snapshot.refusals[0].size == sizeof(weivel_declaration_refusal));
        check_text(snapshot.refusals[0].subject, "elsewhere:thing", "ledger refusal subject");
        CHECK(snapshot.refusals[0].has_fix == 1);
    }
    CHECK(weivel_ledger_snapshot_release(&snapshot) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

static weivel_core *core_for_api(uint32_t api) {
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.api = api;
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    return core;
}

static void an_unsupported_manifest_names_its_import_name(void) {
    weivel_core *core = core_for_api(1);
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "future";
    manifest.api = 9;
    weivel_support answer = WEIVEL_INIT(weivel_support);
    CHECK(weivel_supports(core, &manifest, &answer, NULL) == WEIVEL_STATUS_OK);
    CHECK(answer.refusal.code == WEIVEL_REFUSAL_INCOMPATIBLE);
    check_text(answer.subject, "future", "incompatible subject");
    CHECK(answer.has_fix == 1);
    CHECK(weivel_support_release(&answer) == WEIVEL_STATUS_OK);

    manifest.api = 1;
    CHECK(weivel_supports(core, &manifest, &answer, NULL) == WEIVEL_STATUS_OK);
    CHECK(answer.supported == 1);
    CHECK(answer.subject == NULL);
    CHECK(answer.has_fix == 0);
    CHECK(weivel_support_release(&answer) == WEIVEL_STATUS_OK);

    const char *const targets[] = {NULL};
    manifest.targets.items = targets;
    manifest.targets.count = 1;
    CHECK(weivel_supports(core, &manifest, &answer, NULL) == WEIVEL_STATUS_OK);
    CHECK(answer.refusal.code == WEIVEL_REFUSAL_MALFORMED_MANIFEST);
    check_text(answer.subject, "future", "malformed support subject");
    CHECK(weivel_support_release(&answer) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

static void an_old_sized_support_answer_reads_none_of_the_subject(void) {
    weivel_core *core = core_for_api(1);
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "future";
    manifest.api = 9;
    weivel_support answer = WEIVEL_INIT(weivel_support);
    size_t first_shape = offsetof(weivel_support, subject);
    fill_guard((unsigned char *)&answer + first_shape, sizeof answer - first_shape);
    answer.size = (uint32_t)first_shape;
    CHECK(weivel_supports(core, &manifest, &answer, NULL) == WEIVEL_STATUS_OK);
    CHECK(answer.refusal.code == WEIVEL_REFUSAL_INCOMPATIBLE);
    CHECK(guard_intact((unsigned char *)&answer + first_shape, sizeof answer - first_shape));
    CHECK(weivel_support_release(&answer) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

static weivel_handle register_needing(weivel_core *core, const char *import_name, const char *need) {
    weivel_service_want want = WEIVEL_INIT(weivel_service_want);
    want.id = need;
    want.optional = WEIVEL_CHOICE_NO;
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = import_name;
    manifest.api = 1;
    manifest.services.consumes.items = need == NULL ? NULL : &want;
    manifest.services.consumes.count = need == NULL ? 0 : 1;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static void plan_with_three_refusals(weivel_core *core, weivel_activation_plan *plan) {
    weivel_handle listed = register_needing(core, "listed", NULL);
    weivel_handle needy = register_needing(core, "needy", "absent:thing");
    weivel_handle plugins[4] = {(weivel_handle)12345, listed, listed, needy};
    CHECK(weivel_plan_activation(core, plugins, 4, WEIVEL_EDGES_SERVICES, NULL, plan, NULL) == WEIVEL_STATUS_OK);
    CHECK(plan->refusal_count == 3);
}

static void a_plans_subjects_match_its_refusals_one_to_one(void) {
    weivel_core *core = fresh_core();
    weivel_activation_plan plan = WEIVEL_INIT(weivel_activation_plan);
    plan_with_three_refusals(core, &plan);
    CHECK(plan.refusal_subjects != NULL);
    if (plan.refusal_count == 3 && plan.refusal_subjects != NULL) {
        CHECK(plan.refusals[0].refusal.code == WEIVEL_REFUSAL_STALE_HANDLE);
        CHECK(plan.refusal_subjects[0].subject == NULL);
        CHECK(plan.refusals[1].refusal.code == WEIVEL_REFUSAL_REPEATED_PLUGIN);
        check_text(plan.refusal_subjects[1].subject, "listed", "repeated subject");
        CHECK(plan.refusals[2].refusal.code == WEIVEL_REFUSAL_UNMET_NEED);
        check_text(plan.refusal_subjects[2].subject, "absent:thing", "unmet need subject");
        for (uint32_t index = 0; index < 3; index++) {
            CHECK(plan.refusal_subjects[index].has_fix == 1);
        }
    }
    CHECK(weivel_activation_plan_release(&plan) == WEIVEL_STATUS_OK);

    weivel_handle alone = register_needing(core, "alone", NULL);
    CHECK(weivel_plan_activation(core, &alone, 1, WEIVEL_EDGES_SERVICES, NULL, &plan, NULL) == WEIVEL_STATUS_OK);
    CHECK(plan.refusal_count == 0);
    CHECK(plan.refusal_subjects == NULL);
    CHECK(weivel_activation_plan_release(&plan) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

static void an_old_sized_plan_reads_none_of_the_subjects(void) {
    weivel_core *core = fresh_core();
    weivel_activation_plan plan = WEIVEL_INIT(weivel_activation_plan);
    size_t first_shape = offsetof(weivel_activation_plan, refusal_subjects);
    fill_guard((unsigned char *)&plan + first_shape, sizeof plan - first_shape);
    plan.size = (uint32_t)first_shape;
    plan_with_three_refusals(core, &plan);
    CHECK(guard_intact((unsigned char *)&plan + first_shape, sizeof plan - first_shape));
    CHECK(weivel_activation_plan_release(&plan) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

int main(void) {
    a_want_for_an_undeclared_id_names_the_id();
    a_timed_out_and_a_stopped_want_name_the_service();
    a_served_want_names_nothing();
    a_taken_name_identity_and_state_name_the_plugin();
    a_malformed_manifest_names_its_import_name();
    a_malformed_call_and_a_stale_handle_name_nothing();
    a_refused_registration_names_the_service();
    a_refused_activation_names_the_import_name_in_its_event_and_its_row();
    a_quarantine_reason_names_nothing_and_has_a_fix_only_when_given_one();
    an_old_sized_row_reads_none_of_the_subject();
    a_ledger_refusal_names_the_id_it_reached_for();
    an_unsupported_manifest_names_its_import_name();
    an_old_sized_support_answer_reads_none_of_the_subject();
    a_plans_subjects_match_its_refusals_one_to_one();
    an_old_sized_plan_reads_none_of_the_subjects();
    return check_summary("subject_test");
}
