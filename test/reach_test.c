#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <weivel/core.h>

#include "core_internal.h"
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
        record->status = weivel_quarantine_plugin(core, plugin, NULL, NULL);
        break;
    }
    drain(core);
    record->journal_to = the_journal.count;
}

static const weivel_plugin *record_of(weivel_core *core, weivel_handle handle) {
    void *record = NULL;
    CHECK(weivel_handles_resolve(core->handles, handle, WEIVEL_RECORD_PLUGIN, &record) == WEIVEL_STATUS_OK);
    return record;
}

static bool reported(weivel_core *core, weivel_handle plugin, const char *id) {
    const weivel_plugin *record = record_of(core, plugin);
    for (uint32_t index = 0; record != NULL && index < weivel_reach_reported_count(record); index++) {
        if (strcmp(weivel_reach_reported_at(record, index)->id, id) == 0) {
            return true;
        }
    }
    return false;
}

static int by_sequence(const void *left, const void *right) {
    uint64_t first = (*(const weivel_reach_record *const *)left)->sequence;
    uint64_t second = (*(const weivel_reach_record *const *)right)->sequence;
    return first < second ? -1 : first > second;
}

/** Every plugin's reported refusals, merged into the order they were reported. */
static uint32_t all_reported(weivel_core *core, const weivel_handle *plugins, uint32_t count,
                             const weivel_reach_record **into, uint32_t capacity) {
    uint32_t total = 0;
    for (uint32_t plugin = 0; plugin < count; plugin++) {
        const weivel_plugin *record = record_of(core, plugins[plugin]);
        for (uint32_t index = 0; record != NULL && index < weivel_reach_reported_count(record) && total < capacity;
             index++) {
            into[total++] = weivel_reach_reported_at(record, index);
        }
    }
    qsort((void *)into, total, sizeof into[0], by_sequence);
    return total;
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
    const weivel_reach_record *records_reported[16];
    uint32_t reported_count = all_reported(core, plugins, plugin_count(vector), records_reported, 16);
    for (uint32_t index = 0; index < reported_count; index++) {
        const weivel_reach_record *record = records_reported[index];
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
    CHECK(weivel_reach_reported_count(record_of(core, plugin)) == 0);
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

static char *repeated(char letter, size_t count, const char *prefix) {
    size_t prefix_length = strlen(prefix);
    char *text = malloc(prefix_length + count + 1);
    memcpy(text, prefix, prefix_length);
    memset(text + prefix_length, letter, count);
    text[prefix_length + count] = '\0';
    return text;
}

/** Drains every event, counting the reach refusals, settlements and logs that carry `id` whole. */
static uint32_t whole_mentions(weivel_core *core, const char *id) {
    uint32_t whole = 0;
    weivel_event event = WEIVEL_INIT(weivel_event);
    while (weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK) {
        const char *text = NULL;
        if (event.kind == WEIVEL_EVENT_REACH_REFUSED) {
            text = event.data.reach.refusal.detail;
            CHECK(strstr(event.data.reach.refusal.fix, id) != NULL);
        } else if (event.kind == WEIVEL_EVENT_WANT_SETTLED) {
            text = event.data.want.refusal.detail;
            CHECK(strstr(event.data.want.refusal.fix, id) != NULL);
        } else if (event.kind == WEIVEL_EVENT_LOG) {
            text = event.data.log.text;
        }
        if (text != NULL && strstr(text, id) != NULL) {
            whole++;
        }
        weivel_event_release(&event);
    }
    return whole;
}

static void a_long_id_is_never_cut_from_a_reach_refusal(void) {
    weivel_core *core = core_knowing(NULL, 0);
    char *service = repeated('s', 1000, "far:");
    char *capability = repeated('c', 1000, "far:");
    char *declared = repeated('d', 1000, "notes:");
    char *bound = repeated('o', 1000, "owner:");
    plugin_spec notes = {.identity = "notes", .consumes = {declared}};
    plugin_spec consumer = {.identity = "bound", .consumes = {bound}, .providers = {"someone"}};
    plugin_spec owner = {.identity = "owner", .provides = {bound}};
    weivel_handle plugin = open_spec(core, &notes);
    weivel_handle binder = open_spec(core, &consumer);
    weivel_handle provider = open_spec(core, &owner);
    register_service(core, provider, bound);
    forget(core);

    weivel_handle got = 0;
    CHECK(weivel_get_service(core, plugin, service, &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(whole_mentions(core, service) == 1);
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_all_capabilities(core, plugin, capability, &list, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(whole_mentions(core, capability) == 1);
    CHECK(weivel_get_service(core, binder, bound, &got, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(whole_mentions(core, bound) == 1);

    want_untimed(core, plugin, declared);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(whole_mentions(core, declared) == 1);
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, plugin, declared, &watch, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(whole_mentions(core, declared) == 1);
    free(bound);
    free(declared);
    free(capability);
    free(service);
    weivel_core_destroy(core);
}

/** Drains every event, counting those whose texts carry `text` whole: a refusal of any kind, or a log. */
static uint32_t carried_whole(weivel_core *core, const char *text) {
    uint32_t whole = 0;
    weivel_event event = WEIVEL_INIT(weivel_event);
    while (weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK) {
        const weivel_refusal *refusal = NULL;
        if (event.kind == WEIVEL_EVENT_REFUSAL) {
            refusal = &event.data.refusal;
        } else if (event.kind == WEIVEL_EVENT_REACH_REFUSED) {
            refusal = &event.data.reach.refusal;
        } else if (event.kind == WEIVEL_EVENT_WANT_SETTLED) {
            refusal = &event.data.want.refusal;
        } else if (event.kind == WEIVEL_EVENT_LOG) {
            whole += strstr(event.data.log.text, text) != NULL;
        }
        if (refusal != NULL && refusal->detail != NULL) {
            whole += strstr(refusal->detail, text) != NULL || strstr(refusal->fix, text) != NULL;
        }
        weivel_event_release(&event);
    }
    return whole;
}

static weivel_status register_named(weivel_core *core, const char *import_name, const char *identity,
                                    weivel_handle *plugin) {
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = import_name;
    manifest.identity = identity;
    manifest.api = 1;
    *plugin = 0;
    return weivel_register_manifest(core, &manifest, plugin, NULL);
}

static void a_long_name_is_never_cut_from_a_plugin_refusal(void) {
    weivel_core *core = core_knowing(NULL, 0);
    char *name = repeated('n', 1000, "plugin");
    char *identity = repeated('i', 1000, "who");
    weivel_handle plugin = 0;
    weivel_handle other = 0;
    CHECK(register_named(core, name, identity, &plugin) == WEIVEL_STATUS_OK);
    forget(core);
    CHECK(register_named(core, name, NULL, &other) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, name) == 1);
    CHECK(register_named(core, "elsewhere", identity, &other) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, identity) == 1);
    weivel_handle registration = 0;
    CHECK(weivel_register_service(core, plugin, "x:y", &registration, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, name) == 1);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, name) == 1);
    CHECK(weivel_verify_activation(core, plugin, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, name) == 1);
    free(identity);
    free(name);
    weivel_core_destroy(core);
}

static void a_long_text_is_never_cut_from_a_manifest_refusal(void) {
    weivel_core *core = core_knowing(NULL, 0);
    char *name = repeated('n', 1000, "plugin");
    char *value = repeated('v', 1000, "value");
    char *service = repeated('s', 1000, "plugin:");
    const char *twice[] = {value, value};
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = name;
    manifest.api = 1;
    manifest.targets.items = twice;
    manifest.targets.count = 2;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    the_journal.count = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_event event = WEIVEL_INIT(weivel_event);
    CHECK(weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK);
    CHECK(event.kind == WEIVEL_EVENT_REFUSAL && strstr(event.data.refusal.detail, name) != NULL &&
          strstr(event.data.refusal.detail, value) != NULL && strstr(event.data.refusal.fix, value) != NULL);
    weivel_event_release(&event);

    weivel_support answer = WEIVEL_INIT(weivel_support);
    CHECK(weivel_supports(core, &manifest, &answer, NULL) == WEIVEL_STATUS_OK);
    CHECK(answer.supported == 0 && strstr(answer.refusal.detail, value) != NULL &&
          strstr(answer.refusal.detail, name) != NULL);
    weivel_support_release(&answer);

    manifest.targets.count = 0;
    manifest.identity = "";
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, name) == 1);
    manifest.identity = NULL;

    weivel_service_want want = WEIVEL_INIT(weivel_service_want);
    want.id = service;
    want.optional = 7;
    manifest.services.consumes.items = &want;
    manifest.services.consumes.count = 1;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, service) == 1);
    want.optional = 0;
    weivel_number numbers[2] = {WEIVEL_INIT(weivel_number), WEIVEL_INIT(weivel_number)};
    numbers[0].name = value;
    numbers[1].name = value;
    want.constraints.items = numbers;
    want.constraints.count = 2;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_event constraints = WEIVEL_INIT(weivel_event);
    CHECK(weivel_next_event(core, &constraints, NULL) == WEIVEL_STATUS_OK);
    CHECK(constraints.kind == WEIVEL_EVENT_REFUSAL && strstr(constraints.data.refusal.detail, service) != NULL &&
          strstr(constraints.data.refusal.detail, value) != NULL);
    weivel_event_release(&constraints);
    manifest.services.consumes.count = 0;

    weivel_service_offer offer = WEIVEL_INIT(weivel_service_offer);
    offer.id = service;
    offer.attributes.items = numbers;
    offer.attributes.count = 2;
    manifest.services.provides.items = &offer;
    manifest.services.provides.count = 1;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_event attributes = WEIVEL_INIT(weivel_event);
    CHECK(weivel_next_event(core, &attributes, NULL) == WEIVEL_STATUS_OK);
    CHECK(attributes.kind == WEIVEL_EVENT_REFUSAL && strstr(attributes.data.refusal.detail, service) != NULL &&
          strstr(attributes.data.refusal.detail, value) != NULL);
    weivel_event_release(&attributes);
    free(service);
    free(value);
    free(name);
    weivel_core_destroy(core);
}

static void a_long_text_is_never_cut_from_a_registry_refusal(void) {
    char *known_first = repeated('k', 1000, "known");
    char *known_last = repeated('l', 1000, "last");
    const char *known[] = {known_first, known_last};
    weivel_core *core = core_knowing(known, 2);
    char *outside = repeated('o', 1000, "outside:");
    char *owned = repeated('w', 1000, "owner:");
    char *identity = repeated('i', 1000, "who");
    char *later = repeated('t', 1000, "owner:");
    plugin_spec owner = {.identity = identity, .import_name = "owner"};
    weivel_handle provider = open_spec(core, &owner);
    plugin_spec rival_spec = {.identity = "rival"};
    weivel_handle rival = open_spec(core, &rival_spec);
    forget(core);

    weivel_handle registration = 0;
    CHECK(weivel_register_service(core, provider, outside, &registration, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, outside) == 1);
    CHECK(weivel_register_service(core, provider, outside, &registration, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, known_last) == 1);
    register_service(core, provider, known_first);
    forget(core);
    CHECK(weivel_register_service(core, rival, known_first, &registration, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, identity) == 1);
    CHECK(weivel_register_service(core, rival, known_first, &registration, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(carried_whole(core, known_first) == 1);

    plugin_spec waiting_spec = {.identity = "waiting", .consumes = {owned, later}};
    weivel_handle waiting = open_spec(core, &waiting_spec);
    want_within(core, waiting, owned, 0, 5);
    CHECK(weivel_advance(core, 5, NULL) == WEIVEL_STATUS_OK);
    CHECK(carried_whole(core, owned) == 1);

    free(later);
    free(identity);
    free(owned);
    free(outside);
    weivel_core_destroy(core);
    free(known_last);
    free(known_first);
}

static void a_long_id_is_never_cut_from_an_unpolled_want(void) {
    weivel_core *core = core_knowing(NULL, 0);
    char *service = repeated('c', 1000, "far:");
    weivel_number constraint = WEIVEL_INIT(weivel_number);
    constraint.name = "latency";
    constraint.value = 5;
    weivel_service_want want = WEIVEL_INIT(weivel_service_want);
    want.id = service;
    want.constraints.items = &constraint;
    want.constraints.count = 1;
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "picky";
    manifest.api = 1;
    manifest.services.consumes.items = &want;
    manifest.services.consumes.count = 1;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    forget(core);
    weivel_handle handle = 0;
    CHECK(weivel_want_service(core, plugin, service, NULL, &handle, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(carried_whole(core, service) == 1);
    free(service);
    weivel_core_destroy(core);
}

static uint32_t ledger_has_fix(weivel_core *core) {
    weivel_ledger_snapshot snapshot = WEIVEL_INIT(weivel_ledger_snapshot);
    CHECK(weivel_ledger(core, &snapshot, NULL) == WEIVEL_STATUS_OK);
    CHECK(snapshot.refusal_count == 1);
    uint32_t has_fix = snapshot.refusal_count == 1 ? snapshot.refusals[0].has_fix : 2;
    CHECK(weivel_ledger_snapshot_release(&snapshot) == WEIVEL_STATUS_OK);
    return has_fix;
}

static void a_ledger_refusal_answers_has_fix_from_the_fix_its_record_keeps(void) {
    weivel_core *core = core_knowing(NULL, 0);
    plugin_spec spec = {.identity = "asker"};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_handle implementation = 0;
    CHECK(weivel_get_service(core, plugin, "elsewhere:thing", &implementation, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_reach_record *record = (weivel_reach_record *)weivel_reach_reported_at(record_of(core, plugin), 0);
    CHECK(record != NULL && record->fix != NULL && record->fix[0] != '\0');

    CHECK(ledger_has_fix(core) == 1);
    if (record != NULL && record->fix != NULL) {
        record->fix[0] = '\0';
    }
    CHECK(ledger_has_fix(core) == 0);
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
    a_long_id_is_never_cut_from_a_reach_refusal();
    a_long_name_is_never_cut_from_a_plugin_refusal();
    a_long_text_is_never_cut_from_a_manifest_refusal();
    a_long_text_is_never_cut_from_a_registry_refusal();
    a_long_id_is_never_cut_from_an_unpolled_want();
    a_ledger_refusal_answers_has_fix_from_the_fix_its_record_keeps();
    return check_summary("reach_test");
}
