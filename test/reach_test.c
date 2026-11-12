#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <weivel/core.h>

#include "registry.h"
#include "registry_support.h"

typedef enum step_kind { REGISTER, GET, WANT, WATCH, IDS, RELEASE, QUARANTINE } step_kind;

typedef struct step {
    uint32_t plugin;
    step_kind kind;
    const char *id;
} step;

#define MOST_PLUGINS 3
#define MOST_STEPS 6

/** A reach vector: the host's services, the plugins opened in order, and the steps, each made by one plugin. */
typedef struct reach_vector {
    const char *host[3];
    plugin_spec plugins[MOST_PLUGINS];
    step steps[MOST_STEPS];
} reach_vector;

/** The vector's answer: outcomes joined by commas, reported pairs as "plugin id;", and the first refusal's text. */
typedef struct reach_answer {
    char outcomes[512];
    char reported[512];
    char refusal[1100];
} reach_answer;

typedef struct step_record {
    step_kind kind;
    weivel_status status;
    weivel_handle handle;
    uint32_t plugin;
    const char *id;
    uint32_t journal_from;
    uint32_t journal_to;
    char ids[256];
} step_record;

static uint32_t plugin_count(const reach_vector *vector) {
    uint32_t count = 0;
    while (count < MOST_PLUGINS && vector->plugins[count].identity != NULL) {
        count++;
    }
    return count;
}

static int compare_texts(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static void list_sorted(weivel_core *core, weivel_handle plugin, char *into, size_t capacity) {
    weivel_service_list list = WEIVEL_INIT(weivel_service_list);
    CHECK(weivel_service_ids(core, plugin, &list, NULL) == WEIVEL_STATUS_OK);
    const char *sorted[16];
    uint32_t count = list.ids.count < 16 ? list.ids.count : 16;
    for (uint32_t index = 0; index < count; index++) {
        sorted[index] = list.ids.items[index];
    }
    qsort((void *)sorted, count, sizeof sorted[0], compare_texts);
    size_t used = (size_t)snprintf(into, capacity, "[");
    for (uint32_t index = 0; index < count; index++) {
        used += (size_t)snprintf(into + used, capacity - used, "%s%s", index == 0 ? "" : " ", sorted[index]);
    }
    snprintf(into + used, capacity - used, "]");
    CHECK(weivel_service_list_release(&list) == WEIVEL_STATUS_OK);
}

static void perform(weivel_core *core, const weivel_handle *plugins, const step *the_step, step_record *record) {
    weivel_handle plugin = plugins[the_step->plugin];
    record->kind = the_step->kind;
    record->plugin = the_step->plugin;
    record->id = the_step->id;
    record->journal_from = the_journal.count;
    switch (the_step->kind) {
    case REGISTER:
        record->status = weivel_register_service(core, plugin, the_step->id, &record->handle, NULL);
        break;
    case GET:
        record->status = weivel_get_service(core, plugin, the_step->id, &record->handle, NULL);
        break;
    case WANT:
        record->status = weivel_want_service(core, plugin, the_step->id, NULL, &record->handle, NULL);
        break;
    case WATCH:
        record->status = weivel_watch_service(core, plugin, the_step->id, &record->handle, NULL);
        break;
    case IDS:
        list_sorted(core, plugin, record->ids, sizeof record->ids);
        break;
    case RELEASE:
        record->status = weivel_release_plugin(core, plugin, NULL);
        break;
    case QUARANTINE:
        record->status = weivel_quarantine_plugin(core, plugin, NULL);
        break;
    }
    drain(core);
    record->journal_to = the_journal.count;
}

static bool reported(weivel_core *core, weivel_handle plugin, const char *id) {
    for (uint32_t index = 0; index < weivel_reach_reported_count(core); index++) {
        const weivel_reach_record *record = weivel_reach_reported_at(core, index);
        if (record->plugin == plugin && strcmp(record->id, id) == 0) {
            return true;
        }
    }
    return false;
}

static const char *outcome_of(weivel_core *core, const weivel_handle *plugins, const step_record *record) {
    weivel_handle plugin = plugins[record->plugin];
    if (record->kind == GET) {
        if (record->status == WEIVEL_STATUS_OK) {
            return "served";
        }
        return reported(core, plugin, record->id) ? "refused" : "absent";
    }
    if (record->kind == WANT) {
        const seen *settled = settlement_of(record->handle);
        if (settled == NULL) {
            return "absent";
        }
        return settled->implementation != 0 ? "served" : "refused";
    }
    if (record->handle != 0 && heard_by(record->handle, WEIVEL_CHANGE_REGISTERED) > 0) {
        return "served";
    }
    return reported(core, plugin, record->id) ? "refused" : "absent";
}

/** The refusal text one step produced: a want's rejection, or the report a get or watch emitted. */
static bool refusal_of(const step_record *record, char *into, size_t capacity) {
    if (record->kind == WANT) {
        const seen *settled = settlement_of(record->handle);
        if (settled != NULL && settled->implementation == 0) {
            snprintf(into, capacity, "%s: %s", settled->detail, settled->fix);
            return true;
        }
        return false;
    }
    if (record->kind != GET && record->kind != WATCH) {
        return false;
    }
    for (uint32_t at = record->journal_from; at < record->journal_to; at++) {
        const seen *item = &the_journal.items[at];
        if (item->kind == WEIVEL_EVENT_REACH_REFUSED) {
            snprintf(into, capacity, "%s: %s", item->detail, item->fix);
            return true;
        }
    }
    return false;
}

static void answer_of(weivel_core *core, const reach_vector *vector, const weivel_handle *plugins,
                      const step_record *records, uint32_t count, reach_answer *answer) {
    size_t used = 0;
    answer->outcomes[0] = '\0';
    answer->refusal[0] = '\0';
    for (uint32_t index = 0; index < count; index++) {
        const step_record *record = &records[index];
        if (record->kind == REGISTER || record->kind == RELEASE || record->kind == QUARANTINE) {
            continue;
        }
        const char *outcome = record->kind == IDS ? record->ids : outcome_of(core, plugins, record);
        used += (size_t)snprintf(answer->outcomes + used, sizeof answer->outcomes - used, "%s%s",
                                 used == 0 ? "" : ",", outcome);
        if (answer->refusal[0] == '\0') {
            refusal_of(record, answer->refusal, sizeof answer->refusal);
        }
    }
    used = 0;
    answer->reported[0] = '\0';
    for (uint32_t index = 0; index < weivel_reach_reported_count(core); index++) {
        const weivel_reach_record *record = weivel_reach_reported_at(core, index);
        const char *name = "?";
        for (uint32_t plugin = 0; plugin < plugin_count(vector); plugin++) {
            if (plugins[plugin] == record->plugin) {
                name = vector->plugins[plugin].identity;
            }
        }
        CHECK(record->kind == WEIVEL_REACH_SERVICE);
        used += (size_t)snprintf(answer->reported + used, sizeof answer->reported - used, "%s %s;", name, record->id);
    }
}

static void run(const reach_vector *vector, reach_answer *answer) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle plugins[MOST_PLUGINS] = {0};
    for (uint32_t index = 0; index < plugin_count(vector); index++) {
        plugins[index] = open_spec(core, &vector->plugins[index]);
    }
    for (uint32_t index = 0; index < 3 && vector->host[index] != NULL; index++) {
        register_service(core, WEIVEL_HOST, vector->host[index]);
    }
    forget(core);
    step_record records[MOST_STEPS];
    uint32_t count = 0;
    while (count < MOST_STEPS && vector->steps[count].id != NULL) {
        perform(core, plugins, &vector->steps[count], &records[count]);
        count++;
    }
    answer_of(core, vector, plugins, records, count, answer);
    weivel_core_destroy(core);
}

static void check_mentions_or_none(const reach_answer *answer, const char *first, const char *second) {
    if (first == NULL) {
        check_text(answer->refusal, "", "refusal");
        return;
    }
    mentions(answer->refusal, first);
    mentions(answer->refusal, second);
}

#define SERVICES_CONSUMED "the services it consumes"

static void delivers_a_late_registration_from_the_named_provider_to_a_want(void) {
    reach_vector vector = {
        {NULL},
        {{"forge:2", "calendar", {NULL}, {NULL}, {"calendar:events"}},
         {"agenda", NULL, {"calendar:events"}, {"forge:2"}, {NULL}}},
        {{1, WANT, "calendar:events"}, {0, REGISTER, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "served", "outcomes");
    check_text(answer.reported, "", "reported");
}

static void delivers_a_late_registration_from_the_named_provider_to_a_watch(void) {
    reach_vector vector = {
        {NULL},
        {{"forge:2", "calendar", {NULL}, {NULL}, {"calendar:events"}},
         {"agenda", NULL, {"calendar:events"}, {"forge:2"}, {NULL}}},
        {{1, WATCH, "calendar:events"}, {0, REGISTER, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "served", "outcomes");
    check_text(answer.reported, "", "reported");
}

static void does_not_deliver_a_late_registration_from_the_wrong_provider_to_a_want(void) {
    reach_vector vector = {
        {NULL},
        {{"forge:2", "calendar", {NULL}, {NULL}, {"calendar:events"}},
         {"agenda", NULL, {"calendar:events"}, {"forge:1"}, {NULL}}},
        {{1, WANT, "calendar:events"}, {0, REGISTER, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "absent", "outcomes");
    check_text(answer.reported, "agenda calendar:events;", "reported");
    check_mentions_or_none(&answer, NULL, NULL);
}

static void does_not_deliver_a_late_registration_from_the_wrong_provider_to_a_watch(void) {
    reach_vector vector = {
        {NULL},
        {{"forge:2", "calendar", {NULL}, {NULL}, {"calendar:events"}},
         {"agenda", NULL, {"calendar:events"}, {"forge:1"}, {NULL}}},
        {{1, WATCH, "calendar:events"}, {0, REGISTER, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "absent", "outcomes");
    check_text(answer.reported, "", "reported");
    check_mentions_or_none(&answer, NULL, NULL);
}

static void lets_a_plugin_reach_what_it_provides(void) {
    reach_vector vector = {{NULL},
                           {{"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}}},
                           {{0, WATCH, "calendar:events"},
                            {0, REGISTER, "calendar:events"},
                            {0, GET, "calendar:events"},
                            {0, WANT, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "served,served,served", "outcomes");
    check_text(answer.reported, "", "reported");
    check_mentions_or_none(&answer, NULL, NULL);
}

static void lists_only_the_ids_a_plugin_may_reach(void) {
    reach_vector vector = {{"clock", "locale", NULL},
                           {{"calendar", NULL, {"clock", "weather:forecast"}, {NULL}, {"calendar:events"}},
                            {"notes", NULL, {"calendar:events"}, {NULL}, {NULL}},
                            {"idle", NULL, {NULL}, {NULL}, {NULL}}},
                           {{0, REGISTER, "calendar:events"}, {0, IDS, "-"}, {1, IDS, "-"}, {2, IDS, "-"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "[calendar:events clock],[calendar:events],[]", "outcomes");
    check_text(answer.reported, "", "reported");
}

static void reaches_an_id_declared_in_the_object_form(void) {
    reach_vector vector = {{"clock", NULL},
                           {{"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}},
                            {"agenda", NULL, {"calendar:events"}, {NULL}, {NULL}}},
                           {{0, REGISTER, "calendar:events"},
                            {1, GET, "calendar:events"},
                            {1, GET, "clock"},
                            {1, IDS, "-"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "served,refused,[calendar:events]", "outcomes");
    check_text(answer.reported, "agenda clock;", "reported");
    check_mentions_or_none(&answer, SERVICES_CONSUMED, "clock");
}

static void reads_a_declared_id_nothing_provides_as_absent(void) {
    reach_vector vector = {
        {NULL},
        {{"notes", NULL, {"calendar:events"}, {NULL}, {NULL}}},
        {{0, GET, "calendar:events"}, {0, WANT, "calendar:events"}, {0, WATCH, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "absent,absent,absent", "outcomes");
    check_text(answer.reported, "", "reported");
    check_mentions_or_none(&answer, NULL, NULL);
}

static void refuses_a_host_service_the_plugin_did_not_declare(void) {
    reach_vector vector = {{"clock", NULL},
                           {{"notes", NULL, {NULL}, {NULL}, {NULL}}, {"agenda", NULL, {"clock"}, {NULL}, {NULL}}},
                           {{0, GET, "clock"}, {1, GET, "clock"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "refused,served", "outcomes");
    check_text(answer.reported, "notes clock;", "reported");
    check_mentions_or_none(&answer, SERVICES_CONSUMED, "clock");
}

static void refuses_a_service_from_the_wrong_provider(void) {
    reach_vector vector = {
        {NULL},
        {{"forge:2", "calendar", {NULL}, {NULL}, {"calendar:events"}},
         {"agenda", NULL, {"calendar:events"}, {"forge:1"}, {NULL}}},
        {{0, REGISTER, "calendar:events"}, {1, GET, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "refused", "outcomes");
    check_text(answer.reported, "agenda calendar:events;", "reported");
    check_mentions_or_none(&answer, "forge:1", "forge:2");
}

static void refuses_an_undeclared_get_and_reports_it_once(void) {
    reach_vector vector = {{NULL},
                           {{"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}},
                            {"notes", NULL, {NULL}, {NULL}, {NULL}},
                            {"agenda", NULL, {"calendar:events"}, {NULL}, {NULL}}},
                           {{0, REGISTER, "calendar:events"},
                            {1, GET, "calendar:events"},
                            {1, GET, "calendar:events"},
                            {2, GET, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "refused,refused,served", "outcomes");
    check_text(answer.reported, "notes calendar:events;", "reported");
    check_mentions_or_none(&answer, SERVICES_CONSUMED, "calendar:events");
}

static void refuses_an_undeclared_want(void) {
    reach_vector vector = {
        {NULL},
        {{"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}},
         {"notes", NULL, {NULL}, {NULL}, {NULL}},
         {"agenda", NULL, {"calendar:events"}, {NULL}, {NULL}}},
        {{0, REGISTER, "calendar:events"}, {1, WANT, "calendar:events"}, {2, WANT, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "refused,served", "outcomes");
    check_text(answer.reported, "notes calendar:events;", "reported");
    check_mentions_or_none(&answer, SERVICES_CONSUMED, "calendar:events");
}

static void refuses_an_undeclared_watch(void) {
    reach_vector vector = {
        {NULL},
        {{"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}},
         {"notes", NULL, {NULL}, {NULL}, {NULL}},
         {"agenda", NULL, {"calendar:events"}, {NULL}, {NULL}}},
        {{1, WATCH, "calendar:events"}, {2, WATCH, "calendar:events"}, {0, REGISTER, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "refused,served", "outcomes");
    check_text(answer.reported, "notes calendar:events;", "reported");
    check_mentions_or_none(&answer, SERVICES_CONSUMED, "calendar:events");
}

static void serves_a_service_from_the_named_provider(void) {
    reach_vector vector = {
        {NULL},
        {{"forge:2", "calendar", {NULL}, {NULL}, {"calendar:events"}},
         {"agenda", NULL, {"calendar:events"}, {"forge:2"}, {NULL}}},
        {{0, REGISTER, "calendar:events"}, {1, GET, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "served", "outcomes");
    check_text(answer.reported, "", "reported");
}

static void serves_an_unbound_consumer_from_anyone(void) {
    reach_vector vector = {
        {NULL},
        {{"forge:2", "calendar", {NULL}, {NULL}, {"calendar:events"}},
         {"agenda", NULL, {"calendar:events"}, {NULL}, {NULL}}},
        {{0, REGISTER, "calendar:events"}, {1, GET, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "served", "outcomes");
    check_text(answer.reported, "", "reported");
}

static void an_undeclared_call_from_a_released_plugin_is_refused_as_undeclared(void) {
    reach_vector vector = {{NULL},
                           {{"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}},
                            {"notes", NULL, {NULL}, {NULL}, {NULL}}},
                           {{0, REGISTER, "calendar:events"},
                            {1, RELEASE, "-"},
                            {1, GET, "calendar:events"},
                            {1, WANT, "calendar:events"},
                            {1, WATCH, "calendar:events"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "refused,refused,refused", "outcomes");
    check_text(answer.reported, "notes calendar:events;", "reported");
    check_mentions_or_none(&answer, SERVICES_CONSUMED, "calendar:events");
}

static void a_declared_late_get_is_let_through_and_a_late_want_rejected(void) {
    reach_vector vector = {{NULL},
                           {{"calendar", NULL, {NULL}, {NULL}, {"calendar:events"}},
                            {"notes", NULL, {"calendar:events"}, {NULL}, {NULL}}},
                           {{0, REGISTER, "calendar:events"},
                            {1, QUARANTINE, "-"},
                            {1, GET, "calendar:events"},
                            {1, WANT, "calendar:events"},
                            {1, WATCH, "calendar:events"},
                            {1, IDS, "-"}}};
    reach_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "served,refused,absent,[calendar:events]", "outcomes");
    check_text(answer.reported, "", "reported");
    mentions(answer.refusal, "stopped while waiting for service \"calendar:events\"");
}

static void late_calls_are_logged_and_never_reported(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec notes = {"notes", NULL, {"calendar:events"}, {NULL}, {"notes:pad"}};
    weivel_handle plugin = open_spec(core, &notes);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    forget(core);
    weivel_handle watch = 1;
    CHECK(weivel_watch_service(core, plugin, "calendar:events", &watch, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(watch == 0);
    weivel_handle registration = 1;
    CHECK(weivel_register_service(core, plugin, "notes:pad", &registration, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(registration == 0);
    weivel_handle want = want_untimed(core, plugin, "calendar:events");
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_LOG) == 3);
    CHECK(count_kind(WEIVEL_EVENT_REFUSAL) == 0);
    CHECK(count_kind(WEIVEL_EVENT_REACH_REFUSED) == 0);
    CHECK(weivel_reach_reported_count(core) == 0);
    check_text(nth_of(WEIVEL_EVENT_LOG, 0)->detail,
               "ignored a late watch of service \"calendar:events\" from notes, which is no longer running", "log");
    check_text(nth_of(WEIVEL_EVENT_LOG, 1)->detail,
               "ignored a late registration of service \"notes:pad\" from notes, which is no longer running", "log");
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->code == WEIVEL_REFUSAL_STOPPED);
    weivel_core_destroy(core);
}

static void refusal_texts_name_the_services_it_consumes(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec owner = {"forge:2", "calendar", {NULL}, {NULL}, {"calendar:events"}};
    plugin_spec bound = {"agenda", NULL, {"calendar:events"}, {"forge:1"}, {NULL}};
    plugin_spec stranger = {"notes", NULL, {NULL}, {NULL}, {NULL}};
    weivel_handle provider = open_spec(core, &owner);
    weivel_handle consumer = open_spec(core, &bound);
    weivel_handle other = open_spec(core, &stranger);
    register_service(core, provider, "calendar:events");
    forget(core);
    weivel_handle got = 1;
    CHECK(weivel_get_service(core, other, "calendar:events", &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(got == 0);
    CHECK(weivel_get_service(core, consumer, "calendar:events", &got, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    const seen *undeclared = nth_of(WEIVEL_EVENT_REACH_REFUSED, 0);
    const seen *mismatch = nth_of(WEIVEL_EVENT_REACH_REFUSED, 1);
    CHECK(undeclared->plugin == other && undeclared->code == WEIVEL_REFUSAL_UNDECLARED);
    CHECK(undeclared->change == WEIVEL_REACH_SERVICE);
    check_text(undeclared->id, "calendar:events", "id");
    check_text(undeclared->detail, "reached for service \"calendar:events\", which it does not declare", "detail");
    check_text(undeclared->fix, "add \"calendar:events\" to the services it consumes in its manifest", "fix");
    CHECK(mismatch->plugin == consumer && mismatch->code == WEIVEL_REFUSAL_WRONG_PROVIDER);
    check_text(mismatch->detail,
               "reached for service \"calendar:events\", which it binds to forge:1 but forge:2 provides", "detail");
    check_text(mismatch->fix,
               "install forge:1, or remove the provider from \"calendar:events\" in the services it consumes in its "
               "manifest",
               "fix");
    weivel_core_destroy(core);
}

int main(void) {
    delivers_a_late_registration_from_the_named_provider_to_a_want();
    delivers_a_late_registration_from_the_named_provider_to_a_watch();
    does_not_deliver_a_late_registration_from_the_wrong_provider_to_a_want();
    does_not_deliver_a_late_registration_from_the_wrong_provider_to_a_watch();
    lets_a_plugin_reach_what_it_provides();
    lists_only_the_ids_a_plugin_may_reach();
    reaches_an_id_declared_in_the_object_form();
    reads_a_declared_id_nothing_provides_as_absent();
    refuses_a_host_service_the_plugin_did_not_declare();
    refuses_a_service_from_the_wrong_provider();
    refuses_an_undeclared_get_and_reports_it_once();
    refuses_an_undeclared_want();
    refuses_an_undeclared_watch();
    serves_a_service_from_the_named_provider();
    serves_an_unbound_consumer_from_anyone();
    an_undeclared_call_from_a_released_plugin_is_refused_as_undeclared();
    a_declared_late_get_is_let_through_and_a_late_want_rejected();
    late_calls_are_logged_and_never_reported();
    refusal_texts_name_the_services_it_consumes();
    return check_summary("reach_test");
}
