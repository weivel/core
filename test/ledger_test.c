#include "registry_support.h"

/** A plugin as the ledger tests need one: services, capabilities and permissions, each list NULL-terminated. */
typedef struct ledger_spec {
    const char *import_name;
    const char *identity;
    const char *consumes[4];
    const char *provides[4];
    const char *capabilities_provided[4];
    const char *capabilities_consumed[4];
    const char *permissions[4];
} ledger_spec;

static weivel_handle register_ledger_spec(weivel_core *core, const ledger_spec *spec) {
    weivel_service_want wants[4];
    weivel_service_offer offers[4];
    uint32_t consumed = count_of(spec->consumes, 4);
    uint32_t provided = count_of(spec->provides, 4);
    for (uint32_t index = 0; index < consumed; index++) {
        weivel_service_want want = WEIVEL_INIT(weivel_service_want);
        want.id = spec->consumes[index];
        wants[index] = want;
    }
    for (uint32_t index = 0; index < provided; index++) {
        weivel_service_offer offer = WEIVEL_INIT(weivel_service_offer);
        offer.id = spec->provides[index];
        offers[index] = offer;
    }
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = spec->import_name;
    manifest.identity = spec->identity;
    manifest.api = 1;
    manifest.services.consumes.items = consumed == 0 ? NULL : wants;
    manifest.services.consumes.count = consumed;
    manifest.services.provides.items = provided == 0 ? NULL : offers;
    manifest.services.provides.count = provided;
    manifest.capabilities.provides.items = spec->capabilities_provided;
    manifest.capabilities.provides.count = count_of(spec->capabilities_provided, 4);
    manifest.capabilities.consumes.items = spec->capabilities_consumed;
    manifest.capabilities.consumes.count = count_of(spec->capabilities_consumed, 4);
    manifest.permissions.items = spec->permissions;
    manifest.permissions.count = count_of(spec->permissions, 4);
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static weivel_handle open_ledger_spec(weivel_core *core, const ledger_spec *spec) {
    weivel_handle plugin = register_ledger_spec(core, spec);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static weivel_core *plain_core(void) {
    return core_knowing(NULL, 0);
}

/** Checks a list against the NULL-terminated texts expected, in order. */
static void check_strings(const weivel_strings *actual, const char *const *expected, const char *label) {
    uint32_t count = count_of(expected, 8);
    if (actual->count != count) {
        fprintf(stderr, "%s: expected %u items, got %u\n", label, count, actual->count);
        check_failures++;
        return;
    }
    CHECK(count == 0 ? actual->items == NULL : actual->items != NULL);
    for (uint32_t index = 0; index < count; index++) {
        check_text(actual->items[index], expected[index], label);
    }
}

static const char *const none[1] = {NULL};

static const weivel_ledger_row *row_at(const weivel_ledger_snapshot *snapshot, uint32_t index) {
    uint32_t stride = snapshot->rows[0].size;
    return (const weivel_ledger_row *)(const void *)((const char *)snapshot->rows + (size_t)index * stride);
}

static const weivel_declaration_refusal *refusal_at(const weivel_ledger_snapshot *snapshot, uint32_t index) {
    uint32_t stride = snapshot->refusals[0].size;
    return (const weivel_declaration_refusal *)(const void *)((const char *)snapshot->refusals +
                                                               (size_t)index * stride);
}

static weivel_ledger_snapshot snapshot_of(weivel_core *core) {
    weivel_ledger_snapshot snapshot = WEIVEL_INIT(weivel_ledger_snapshot);
    CHECK(weivel_ledger(core, &snapshot, NULL) == WEIVEL_STATUS_OK);
    return snapshot;
}

static void lists_each_plugin_in_registration_order_with_what_it_declared(void) {
    weivel_core *core = plain_core();
    ledger_spec screens = {.import_name = "screens", .identity = "forge:screens",
                           .capabilities_provided = {"screen", "panel"}, .permissions = {"network", "disk"}};
    ledger_spec notes = {.import_name = "notes"};
    weivel_handle first = register_ledger_spec(core, &screens);
    weivel_handle second = register_ledger_spec(core, &notes);

    weivel_ledger_snapshot snapshot = snapshot_of(core);
    CHECK(snapshot.row_count == 2);
    CHECK(snapshot.refusal_count == 0 && snapshot.refusals == NULL);
    if (snapshot.row_count == 2) {
        const weivel_ledger_row *row = row_at(&snapshot, 0);
        CHECK(row->size == sizeof(weivel_ledger_row));
        CHECK(row->plugin == first);
        check_text(row->import_name, "screens", "import name");
        check_text(row->identity, "forge:screens", "identity");
        CHECK(row->status == WEIVEL_PLUGIN_STATUS_REGISTERED);
        CHECK(row->verified == 0);
        CHECK(row->refusal.code == WEIVEL_REFUSAL_NONE && row->refusal.detail == NULL);
        const char *const declared[] = {"screen", "panel", NULL};
        const char *const permissions[] = {"network", "disk", NULL};
        check_strings(&row->capabilities_declared, declared, "capabilities declared");
        check_strings(&row->permissions, permissions, "permissions");
        check_strings(&row->services_provided, none, "services provided");
        check_strings(&row->capabilities_provided, none, "capabilities provided");
        check_strings(&row->services_consumed, none, "services consumed");
        CHECK(row->storage == NULL);
        const weivel_ledger_row *other = row_at(&snapshot, 1);
        CHECK(other->plugin == second);
        check_text(other->identity, "notes", "identity defaults to the import name");
    }
    CHECK(weivel_ledger_release(&snapshot) == WEIVEL_STATUS_OK);
    CHECK(snapshot.storage == NULL && snapshot.rows == NULL && snapshot.row_count == 0);
    weivel_core_destroy(core);
}

static void records_what_each_activation_provided_and_consumed(void) {
    weivel_core *core = plain_core();
    ledger_spec provider = {.import_name = "calendar", .provides = {"calendar:events"},
                            .capabilities_provided = {"screen"}};
    ledger_spec consumer = {.import_name = "notes", .consumes = {"calendar:events", "calendar:later"}};
    weivel_handle calendar = open_ledger_spec(core, &provider);
    weivel_handle notes = open_ledger_spec(core, &consumer);

    register_service(core, calendar, "calendar:events");
    weivel_handle implementation = 0;
    CHECK(weivel_provide_capability(core, calendar, "screen", &implementation, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);

    weivel_handle got = 0;
    want_untimed(core, notes, "calendar:later");
    CHECK(weivel_get_service(core, notes, "calendar:events", &got, NULL) == WEIVEL_STATUS_OK);
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, notes, "calendar:events", &watch, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_get_service(core, notes, "calendar:events", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_get_service(core, notes, "elsewhere:thing", &got, NULL) == WEIVEL_STATUS_REFUSED);

    weivel_ledger_row row = WEIVEL_INIT(weivel_ledger_row);
    CHECK(weivel_ledger_entry(core, calendar, &row, NULL) == WEIVEL_STATUS_OK);
    CHECK(row.status == WEIVEL_PLUGIN_STATUS_ACTIVE);
    CHECK(row.verified == 1);
    const char *const services[] = {"calendar:events", NULL};
    const char *const capabilities[] = {"screen", NULL};
    check_strings(&row.services_provided, services, "services provided");
    check_strings(&row.capabilities_provided, capabilities, "capabilities provided");
    CHECK(row.storage != NULL);
    CHECK(weivel_ledger_row_release(&row) == WEIVEL_STATUS_OK);
    CHECK(row.storage == NULL && row.import_name == NULL);

    CHECK(weivel_ledger_entry(core, notes, &row, NULL) == WEIVEL_STATUS_OK);
    const char *const consumed[] = {"calendar:later", "calendar:events", NULL};
    check_strings(&row.services_consumed, consumed, "services consumed, each once, undeclared ones never");
    CHECK(row.verified == 0);
    weivel_ledger_row_release(&row);

    CHECK(weivel_release_plugin(core, calendar, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_ledger_entry(core, calendar, &row, NULL) == WEIVEL_STATUS_OK);
    CHECK(row.status == WEIVEL_PLUGIN_STATUS_RELEASED);
    check_strings(&row.services_provided, services, "a released plugin's row keeps what it provided");
    check_strings(&row.capabilities_provided, capabilities, "and its capabilities");
    weivel_ledger_row_release(&row);

    CHECK(weivel_quarantine_plugin(core, notes, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_ledger_entry(core, notes, &row, NULL) == WEIVEL_STATUS_OK);
    CHECK(row.status == WEIVEL_PLUGIN_STATUS_QUARANTINED);
    check_strings(&row.services_consumed, consumed, "a quarantined plugin's row keeps what it consumed");
    weivel_ledger_row_release(&row);
    CHECK(weivel_open_plugin(core, notes, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_ledger_entry(core, notes, &row, NULL) == WEIVEL_STATUS_OK);
    CHECK(row.status == WEIVEL_PLUGIN_STATUS_ACTIVE);
    check_strings(&row.services_consumed, none, "reopening starts a new activation's relationships");
    weivel_ledger_row_release(&row);
    weivel_core_destroy(core);
}

static void a_late_call_is_no_relationship(void) {
    weivel_core *core = plain_core();
    ledger_spec consumer = {.import_name = "notes", .consumes = {"calendar:events"}};
    weivel_handle notes = open_ledger_spec(core, &consumer);
    CHECK(weivel_release_plugin(core, notes, NULL) == WEIVEL_STATUS_OK);
    want_untimed(core, notes, "calendar:events");
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, notes, "calendar:events", &watch, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_ledger_row row = WEIVEL_INIT(weivel_ledger_row);
    CHECK(weivel_ledger_entry(core, notes, &row, NULL) == WEIVEL_STATUS_OK);
    check_strings(&row.services_consumed, none, "a late want or watch consumed nothing");
    weivel_ledger_row_release(&row);
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, notes, "calendar:events", &got, NULL) == WEIVEL_STATUS_NONE);
    CHECK(weivel_ledger_entry(core, notes, &row, NULL) == WEIVEL_STATUS_OK);
    const char *const asked[] = {"calendar:events", NULL};
    check_strings(&row.services_consumed, asked, "a late get is let through, so it is asked");
    weivel_ledger_row_release(&row);
    weivel_core_destroy(core);
}

static void shows_the_refusal_that_quarantined_an_activation(void) {
    weivel_core *core = plain_core();
    ledger_spec screens = {.import_name = "screens", .capabilities_provided = {"screen"}};
    weivel_handle plugin = open_ledger_spec(core, &screens);
    CHECK(weivel_verify_activation(core, plugin, NULL) == WEIVEL_STATUS_REFUSED);

    weivel_ledger_row row = WEIVEL_INIT(weivel_ledger_row);
    CHECK(weivel_ledger_entry(core, plugin, &row, NULL) == WEIVEL_STATUS_OK);
    CHECK(row.status == WEIVEL_PLUGIN_STATUS_ACTIVE);
    CHECK(row.refusal.code == WEIVEL_REFUSAL_NONE);
    weivel_ledger_row_release(&row);

    CHECK(weivel_quarantine_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_ledger_entry(core, plugin, &row, NULL) == WEIVEL_STATUS_OK);
    CHECK(row.status == WEIVEL_PLUGIN_STATUS_QUARANTINED);
    CHECK(row.verified == 0);
    CHECK(row.refusal.code == WEIVEL_REFUSAL_ACTIVATION_MISMATCH);
    check_text(row.refusal.detail, "capabilities declared but never provided: screen", "quarantine detail");
    check_text(row.refusal.fix,
               "provide \"screen\" while the plugin activates, or remove \"screen\" from the capabilities it "
               "provides in its manifest",
               "quarantine fix");
    weivel_ledger_row_release(&row);

    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    weivel_handle implementation = 0;
    CHECK(weivel_provide_capability(core, plugin, "screen", &implementation, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_verify_activation(core, plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_quarantine_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_ledger_entry(core, plugin, &row, NULL) == WEIVEL_STATUS_OK);
    CHECK(row.status == WEIVEL_PLUGIN_STATUS_QUARANTINED);
    CHECK(row.verified == 1);
    CHECK(row.refusal.code == WEIVEL_REFUSAL_NONE && row.refusal.detail == NULL && row.refusal.fix == NULL);
    weivel_ledger_row_release(&row);
    weivel_core_destroy(core);
}

static void a_forgotten_plugin_is_gone_from_the_ledger(void) {
    weivel_core *core = plain_core();
    ledger_spec first = {.import_name = "first"};
    ledger_spec second = {.import_name = "second"};
    ledger_spec third = {.import_name = "third"};
    weivel_handle a = open_ledger_spec(core, &first);
    weivel_handle b = open_ledger_spec(core, &second);
    weivel_handle c = open_ledger_spec(core, &third);
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, a, "nowhere:x", &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_get_service(core, c, "nowhere:y", &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_release_plugin(core, a, NULL) == WEIVEL_STATUS_OK);

    weivel_ledger_snapshot snapshot = snapshot_of(core);
    CHECK(snapshot.row_count == 3);
    CHECK(snapshot.refusal_count == 2);
    weivel_ledger_release(&snapshot);

    forget(core);
    CHECK(weivel_forget_plugin(core, a, NULL) == WEIVEL_STATUS_OK);
    snapshot = snapshot_of(core);
    CHECK(snapshot.row_count == 2);
    if (snapshot.row_count == 2) {
        CHECK(row_at(&snapshot, 0)->plugin == b);
        CHECK(row_at(&snapshot, 1)->plugin == c);
    }
    CHECK(snapshot.refusal_count == 1);
    if (snapshot.refusal_count == 1) {
        CHECK(refusal_at(&snapshot, 0)->plugin == c);
        check_text(refusal_at(&snapshot, 0)->id, "nowhere:y", "the remaining refusal");
    }
    weivel_ledger_release(&snapshot);

    weivel_ledger_row row = WEIVEL_INIT(weivel_ledger_row);
    CHECK(weivel_ledger_entry(core, a, &row, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(row.storage == NULL);
    drain(core);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_STALE_HANDLE);
    weivel_core_destroy(core);
}

static void lists_refusals_in_the_order_they_were_reported(void) {
    weivel_core *core = plain_core();
    ledger_spec first = {.import_name = "first"};
    ledger_spec second = {.import_name = "second"};
    weivel_handle a = open_ledger_spec(core, &first);
    weivel_handle b = open_ledger_spec(core, &second);
    weivel_handle got = 0;
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_get_service(core, a, "z:one", &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_get_service(core, b, "z:two", &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_capability_all(core, a, "z:one", &list, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_get_service(core, a, "z:one", &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_get_service(core, b, "z:zero", &got, NULL) == WEIVEL_STATUS_REFUSED);

    weivel_ledger_snapshot snapshot = snapshot_of(core);
    CHECK(snapshot.refusal_count == 4);
    if (snapshot.refusal_count == 4) {
        const weivel_handle plugins[] = {a, b, a, b};
        const uint32_t kinds[] = {WEIVEL_REACH_SERVICE, WEIVEL_REACH_SERVICE, WEIVEL_REACH_CAPABILITY,
                                  WEIVEL_REACH_SERVICE};
        const char *const ids[] = {"z:one", "z:two", "z:one", "z:zero"};
        for (uint32_t index = 0; index < 4; index++) {
            const weivel_declaration_refusal *refusal = refusal_at(&snapshot, index);
            CHECK(refusal->size == sizeof(weivel_declaration_refusal));
            CHECK(refusal->plugin == plugins[index]);
            CHECK(refusal->kind == kinds[index]);
            check_text(refusal->id, ids[index], "refusal in report order");
        }
    }
    weivel_ledger_release(&snapshot);
    weivel_core_destroy(core);
}

static void a_snapshot_outlives_the_changes_and_the_core(void) {
    weivel_core *core = plain_core();
    ledger_spec notes = {.import_name = "notes", .consumes = {"notes:x"}};
    weivel_handle plugin = open_ledger_spec(core, &notes);
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, plugin, "notes:x", &got, NULL) == WEIVEL_STATUS_NONE);
    weivel_ledger_snapshot snapshot = snapshot_of(core);
    weivel_ledger_row row = WEIVEL_INIT(weivel_ledger_row);
    CHECK(weivel_ledger_entry(core, plugin, &row, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_forget_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);

    CHECK(snapshot.row_count == 1);
    if (snapshot.row_count == 1) {
        CHECK(row_at(&snapshot, 0)->status == WEIVEL_PLUGIN_STATUS_ACTIVE);
        const char *const consumed[] = {"notes:x", NULL};
        check_strings(&row_at(&snapshot, 0)->services_consumed, consumed, "snapshot after destroy");
    }
    check_text(row.import_name, "notes", "row after destroy");
    CHECK(weivel_ledger_release(&snapshot) == WEIVEL_STATUS_OK);
    CHECK(weivel_ledger_row_release(&row) == WEIVEL_STATUS_OK);
}

static void malformed_ledger_calls_are_answered_as_values(void) {
    weivel_core *core = plain_core();
    weivel_ledger_snapshot snapshot = WEIVEL_INIT(weivel_ledger_snapshot);
    weivel_ledger_row row = WEIVEL_INIT(weivel_ledger_row);
    CHECK(weivel_ledger(NULL, &snapshot, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_ledger(core, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_ledger_snapshot small = snapshot;
    small.size = 4;
    CHECK(weivel_ledger(core, &small, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_ledger_entry(NULL, 1, &row, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_ledger_entry(core, 1, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_ledger_entry(core, 12345, &row, NULL) == WEIVEL_STATUS_REFUSED);

    uint32_t pending = 0;
    CHECK(weivel_ledger(core, &snapshot, &pending) == WEIVEL_STATUS_OK);
    CHECK(snapshot.row_count == 0 && snapshot.rows == NULL && snapshot.storage != NULL);
    CHECK(weivel_ledger(core, &snapshot, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_ledger_release(&snapshot) == WEIVEL_STATUS_OK);
    CHECK(weivel_ledger_release(&snapshot) == WEIVEL_STATUS_OK);

    CHECK(weivel_ledger_release(NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_ledger_release(&small) == WEIVEL_STATUS_MALFORMED);
    weivel_ledger_snapshot zero;
    memset(&zero, 0, sizeof zero);
    CHECK(weivel_ledger_release(&zero) == WEIVEL_STATUS_OK);
    CHECK(weivel_ledger_row_release(NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_ledger_row zero_row;
    memset(&zero_row, 0, sizeof zero_row);
    CHECK(weivel_ledger_row_release(&zero_row) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REFUSAL) == 5);
    weivel_core_destroy(core);
}

/** A row as a caller compiled against a newer header lays it out: one field more than this core knows. */
typedef struct newer_row {
    weivel_ledger_row known;
    uint64_t appended;
} newer_row;

static void a_larger_row_is_written_only_as_far_as_the_core_knows(void) {
    weivel_core *core = plain_core();
    ledger_spec notes = {.import_name = "notes"};
    weivel_handle plugin = register_ledger_spec(core, &notes);
    newer_row row;
    memset(&row, 0xab, sizeof row);
    row.known.size = (uint32_t)sizeof row;
    row.known.storage = NULL;
    CHECK(weivel_ledger_entry(core, plugin, &row.known, NULL) == WEIVEL_STATUS_OK);
    CHECK(row.known.size == sizeof row);
    CHECK(row.appended == 0);
    check_text(row.known.import_name, "notes", "newer row");
    CHECK(weivel_ledger_row_release(&row.known) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

static weivel_core *core_logging_everything(uint32_t initial_capacity) {
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.minimum_log_level = WEIVEL_LOG_DEBUG;
    options.initial_event_capacity = initial_capacity;
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    return core;
}

/** Reads the next event, which must be a log at `level` mentioning `part`. */
static void next_log(weivel_core *core, uint32_t level, const char *part) {
    weivel_event event = WEIVEL_INIT(weivel_event);
    CHECK(weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK);
    CHECK(event.kind == WEIVEL_EVENT_LOG);
    if (event.kind == WEIVEL_EVENT_LOG) {
        CHECK(event.data.log.level == level);
        mentions(event.data.log.text, part);
    }
    weivel_event_release(&event);
}

static void diagnostics_are_log_events_and_none_is_lost_as_the_queue_grows(void) {
    weivel_core *core = core_logging_everything(1);
    ledger_spec notes = {.import_name = "notes", .consumes = {"notes:a"}};
    weivel_handle plugin = open_ledger_spec(core, &notes);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    next_log(core, WEIVEL_LOG_DEBUG, "core created");

    weivel_handle watch = 0;
    for (int index = 0; index < 5; index++) {
        weivel_watch_service(core, plugin, "notes:a", &watch, NULL);
    }
    next_log(core, WEIVEL_LOG_WARNING, "ignored a late watch of service \"notes:a\"");
    next_log(core, WEIVEL_LOG_WARNING, "ignored a late watch");
    uint32_t pending = 0;
    for (int index = 0; index < 300; index++) {
        weivel_watch_service(core, plugin, "notes:a", &watch, &pending);
    }
    CHECK(pending == 303);
    for (uint32_t index = 0; index < 303; index++) {
        next_log(core, WEIVEL_LOG_WARNING, "which is no longer running");
    }
    weivel_event event = WEIVEL_INIT(weivel_event);
    CHECK(weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_NONE);

    CHECK(weivel_advance(core, 7, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_NONE);
    weivel_core_destroy(core);
}

int main(void) {
    lists_each_plugin_in_registration_order_with_what_it_declared();
    records_what_each_activation_provided_and_consumed();
    a_late_call_is_no_relationship();
    shows_the_refusal_that_quarantined_an_activation();
    a_forgotten_plugin_is_gone_from_the_ledger();
    lists_refusals_in_the_order_they_were_reported();
    a_snapshot_outlives_the_changes_and_the_core();
    malformed_ledger_calls_are_answered_as_values();
    a_larger_row_is_written_only_as_far_as_the_core_knows();
    diagnostics_are_log_events_and_none_is_lost_as_the_queue_grows();
    return check_summary("ledger_test");
}
