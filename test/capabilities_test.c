#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <weivel/core.h>

#include "core_internal.h"
#include "registry_support.h"

typedef enum step_kind { END, PROVIDE, VERIFY, ALL, WATCH, QUARANTINE, RELEASE } step_kind;

typedef struct step {
    uint32_t plugin;
    step_kind kind;
    const char *id;
} step;

#define MOST_PLUGINS 4
#define MOST_STEPS 8
#define MOST_IDS 3

/** A plugin as a consume vector describes it: its id, and the capabilities it provides and consumes. */
typedef struct consume_plugin {
    const char *id;
    const char *provides[MOST_IDS];
    const char *consumes[MOST_IDS];
} consume_plugin;

typedef struct consume_vector {
    consume_plugin plugins[MOST_PLUGINS];
    step steps[MOST_STEPS];
} consume_vector;

/**
 * The vector's answer: outcomes and heard each as "[...]" entries joined by commas, reported pairs as
 * "plugin id;", and the first refusal of an `all` or `watch` as "detail: fix".
 */
typedef struct consume_answer {
    char outcomes[512];
    char heard[512];
    char reported[256];
    char refusal[1100];
} consume_answer;

typedef struct step_record {
    step_kind kind;
    weivel_status status;
    weivel_handle watch;
    uint32_t journal_from;
    uint32_t journal_to;
    char outcome[128];
} step_record;

/** What one run leaves for the answer: the plugins' handles and what each one's provide was answered. */
typedef struct consume_run {
    const consume_vector *vector;
    weivel_core *core;
    weivel_handle plugins[MOST_PLUGINS];
    weivel_handle provided[MOST_PLUGINS];
    uint32_t plugin_count;
} consume_run;

static weivel_handle register_capabilities(weivel_core *core, const char *id, const char *const *provides,
                                           const char *const *consumes) {
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = id;
    manifest.api = 1;
    manifest.capabilities.provides.items = provides;
    manifest.capabilities.provides.count = count_of(provides, MOST_IDS);
    manifest.capabilities.consumes.items = consumes;
    manifest.capabilities.consumes.count = count_of(consumes, MOST_IDS);
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static weivel_handle open_capabilities(weivel_core *core, const char *id, const char *const *provides,
                                       const char *const *consumes) {
    weivel_handle plugin = register_capabilities(core, id, provides, consumes);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static const char *name_of(const consume_run *run, weivel_handle plugin) {
    for (uint32_t index = 0; index < run->plugin_count; index++) {
        if (run->plugins[index] == plugin) {
            return run->vector->plugins[index].id;
        }
    }
    return "?";
}

/** The implementation `provider` was answered, so an answer is read off the handles the core handed back. */
static weivel_handle provided_by(const consume_run *run, weivel_handle provider) {
    for (uint32_t index = 0; index < run->plugin_count; index++) {
        if (run->plugins[index] == provider) {
            return run->provided[index];
        }
    }
    return 0;
}

static void read_all(consume_run *run, weivel_handle plugin, const char *id, step_record *record) {
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    record->status = weivel_capability_all(run->core, plugin, id, &list, NULL);
    size_t used = (size_t)snprintf(record->outcome, sizeof record->outcome, "[");
    for (uint32_t index = 0; index < list.count; index++) {
        const weivel_capability_implementation *item = &list.items[index];
        CHECK(item->size == sizeof *item);
        CHECK(item->implementation != 0 && item->implementation == provided_by(run, item->provider));
        used += (size_t)snprintf(record->outcome + used, sizeof record->outcome - used, "%s%s", index == 0 ? "" : " ",
                                 name_of(run, item->provider));
    }
    snprintf(record->outcome + used, sizeof record->outcome - used, "]");
    if (record->status != WEIVEL_STATUS_OK) {
        CHECK(list.count == 0 && list.storage == NULL);
    }
    CHECK(weivel_capability_list_release(&list) == WEIVEL_STATUS_OK);
}

static void perform(consume_run *run, const step *the_step, step_record *record) {
    uint32_t index = the_step->plugin;
    weivel_handle plugin = run->plugins[index];
    record->kind = the_step->kind;
    record->journal_from = the_journal.count;
    switch (the_step->kind) {
    case PROVIDE:
        record->status = weivel_provide_capability(run->core, plugin, the_step->id, &run->provided[index], NULL);
        break;
    case VERIFY:
        record->status = weivel_verify_activation(run->core, plugin, NULL);
        if (record->status == WEIVEL_STATUS_REFUSED) {
            CHECK(weivel_quarantine_plugin(run->core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
        }
        break;
    case ALL:
        read_all(run, plugin, the_step->id, record);
        break;
    case WATCH:
        record->status = weivel_watch_capability(run->core, plugin, the_step->id, &record->watch, NULL);
        break;
    case QUARANTINE:
        record->status = weivel_quarantine_plugin(run->core, plugin, NULL, NULL);
        break;
    case RELEASE:
        record->status = weivel_release_plugin(run->core, plugin, NULL);
        break;
    case END:
        break;
    }
    drain(run->core);
    record->journal_to = the_journal.count;
}

static void heard_of(const consume_run *run, weivel_handle watcher, const step_record *record, char *into,
                     size_t capacity) {
    size_t used = (size_t)snprintf(into, capacity, "[");
    uint32_t heard = 0;
    for (uint32_t at = 0; record->watch != 0 && at < the_journal.count; at++) {
        const seen *item = &the_journal.items[at];
        if (item->kind != WEIVEL_EVENT_CAPABILITY || item->subject != record->watch) {
            continue;
        }
        CHECK(item->plugin == watcher);
        CHECK(item->implementation == provided_by(run, item->provider));
        const char *change = item->change == WEIVEL_CAPABILITY_PROVIDED    ? "provide"
                             : item->change == WEIVEL_CAPABILITY_WITHDRAWN ? "withdraw"
                                                                           : "?";
        used += (size_t)snprintf(into + used, capacity - used, "%s%s %s", heard++ == 0 ? "" : ", ",
                                 name_of(run, item->provider), change);
    }
    snprintf(into + used, capacity - used, "]");
}

static bool refusal_of(const step_record *record, char *into, size_t capacity) {
    if (record->kind != ALL && record->kind != WATCH) {
        return false;
    }
    for (uint32_t at = record->journal_from; at < record->journal_to; at++) {
        const seen *item = &the_journal.items[at];
        if (item->kind == WEIVEL_EVENT_REACH_REFUSED) {
            CHECK(item->change == WEIVEL_REACH_CAPABILITY);
            CHECK(item->code == WEIVEL_REFUSAL_UNDECLARED);
            snprintf(into, capacity, "%s: %s", item->detail, item->fix);
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

static void reported_of(const consume_run *run, char *into, size_t capacity) {
    const weivel_reach_record *records[16];
    uint32_t total = 0;
    for (uint32_t index = 0; index < run->plugin_count; index++) {
        void *found = NULL;
        CHECK(weivel_handles_resolve(run->core->handles, run->plugins[index], WEIVEL_RECORD_PLUGIN, &found) ==
              WEIVEL_STATUS_OK);
        const weivel_plugin *plugin = found;
        for (uint32_t at = 0; plugin != NULL && at < weivel_reach_reported_count(plugin) && total < 16; at++) {
            records[total++] = weivel_reach_reported_at(plugin, at);
        }
    }
    qsort((void *)records, total, sizeof records[0], by_sequence);
    size_t used = 0;
    into[0] = '\0';
    for (uint32_t index = 0; index < total; index++) {
        CHECK(records[index]->kind == WEIVEL_REACH_CAPABILITY);
        used += (size_t)snprintf(into + used, capacity - used, "%s %s;", name_of(run, records[index]->plugin),
                                 records[index]->id);
    }
}

static void append_entry(char *into, size_t capacity, const char *entry) {
    size_t used = strlen(into);
    snprintf(into + used, capacity - used, "%s%s", used == 0 ? "" : ",", entry);
}

static void answer_of(const consume_run *run, const step_record *records, uint32_t count, consume_answer *answer) {
    answer->outcomes[0] = '\0';
    answer->heard[0] = '\0';
    answer->refusal[0] = '\0';
    for (uint32_t index = 0; index < count; index++) {
        const step_record *record = &records[index];
        if (record->kind == ALL) {
            append_entry(answer->outcomes, sizeof answer->outcomes, record->outcome);
        }
        if (record->kind == WATCH) {
            char heard[256];
            heard_of(run, run->plugins[run->vector->steps[index].plugin], record, heard, sizeof heard);
            append_entry(answer->heard, sizeof answer->heard, heard);
        }
        if (answer->refusal[0] == '\0') {
            refusal_of(record, answer->refusal, sizeof answer->refusal);
        }
    }
    reported_of(run, answer->reported, sizeof answer->reported);
}

/**
 * Runs a consume vector as DRIVER.md drives a host: every plugin opened in the order given, every call made as that
 * plugin, and a plugin whose activation check refuses quarantined, as a host does.
 */
static void run(const consume_vector *vector, consume_answer *answer) {
    consume_run state;
    memset(&state, 0, sizeof state);
    state.vector = vector;
    state.core = core_knowing(NULL, 0);
    while (state.plugin_count < MOST_PLUGINS && vector->plugins[state.plugin_count].id != NULL) {
        const consume_plugin *plugin = &vector->plugins[state.plugin_count];
        state.plugins[state.plugin_count] =
            open_capabilities(state.core, plugin->id, plugin->provides, plugin->consumes);
        state.plugin_count++;
    }
    forget(state.core);
    step_record records[MOST_STEPS];
    memset(records, 0, sizeof records);
    uint32_t count = 0;
    while (count < MOST_STEPS && vector->steps[count].kind != END) {
        perform(&state, &vector->steps[count], &records[count]);
        count++;
    }
    answer_of(&state, records, count, answer);
    weivel_core_destroy(state.core);
}

#define CONSUMES_WORDS "the capabilities it consumes"

static void check_refusal_mentions(const consume_answer *answer, const char *id) {
    mentions(answer->refusal, CONSUMES_WORDS);
    mentions(answer->refusal, id);
}

static void a_released_consumer_stops_hearing(void) {
    consume_vector vector = {{{"agenda", {NULL}, {"screens"}}, {"calendar", {"screens"}, {NULL}}},
                             {{0, WATCH, "screens"}, {0, RELEASE, NULL}, {1, PROVIDE, "screens"}, {1, VERIFY, NULL}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.heard, "[]", "heard");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void a_throwing_listener_does_not_silence_the_others(void) {
    consume_vector vector = {
        {{"calendar", {"screens"}, {NULL}}, {"bad", {NULL}, {"screens"}}, {"agenda", {NULL}, {"screens"}}},
        {{1, WATCH, "screens"}, {2, WATCH, "screens"}, {0, PROVIDE, "screens"}, {0, VERIFY, NULL}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.heard, "[calendar provide],[calendar provide]", "heard");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void answers_every_verified_implementation_in_activation_order(void) {
    consume_vector vector = {{{"agenda", {NULL}, {"screens"}},
                              {"calendar", {"screens"}, {NULL}},
                              {"notes", {"screens"}, {NULL}}},
                             {{1, PROVIDE, "screens"},
                              {1, VERIFY, NULL},
                              {2, PROVIDE, "screens"},
                              {2, VERIFY, NULL},
                              {0, ALL, "screens"}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "[calendar notes]", "outcomes");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void answers_in_the_order_activations_were_verified(void) {
    consume_vector vector = {{{"alpha", {"screens"}, {NULL}},
                              {"zulu", {"screens"}, {NULL}},
                              {"mike", {"screens"}, {NULL}},
                              {"reader", {NULL}, {"screens"}}},
                             {{1, PROVIDE, "screens"},
                              {2, PROVIDE, "screens"},
                              {0, PROVIDE, "screens"},
                              {2, VERIFY, NULL},
                              {0, VERIFY, NULL},
                              {1, VERIFY, NULL},
                              {3, ALL, "screens"}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "[mike alpha zulu]", "outcomes");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void does_not_answer_a_provider_whose_activation_was_refused(void) {
    consume_vector vector = {{{"agenda", {NULL}, {"screens"}},
                              {"calendar", {"screens"}, {NULL}},
                              {"flaky", {"screens", "extras"}, {NULL}}},
                             {{1, PROVIDE, "screens"},
                              {1, VERIFY, NULL},
                              {2, PROVIDE, "screens"},
                              {2, VERIFY, NULL},
                              {0, ALL, "screens"}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "[calendar]", "outcomes");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void does_not_answer_an_implementation_before_its_activation_is_verified(void) {
    consume_vector vector = {
        {{"agenda", {NULL}, {"screens"}}, {"calendar", {"screens"}, {NULL}}},
        {{1, PROVIDE, "screens"}, {0, ALL, "screens"}, {1, VERIFY, NULL}, {0, ALL, "screens"}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "[],[calendar]", "outcomes");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void drops_a_released_provider_from_all(void) {
    consume_vector vector = {{{"agenda", {NULL}, {"screens"}},
                              {"calendar", {"screens"}, {NULL}},
                              {"notes", {"screens"}, {NULL}}},
                             {{1, PROVIDE, "screens"},
                              {1, VERIFY, NULL},
                              {2, PROVIDE, "screens"},
                              {2, VERIFY, NULL},
                              {1, RELEASE, NULL},
                              {0, ALL, "screens"}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "[notes]", "outcomes");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void lets_a_plugin_reach_a_capability_it_provides(void) {
    consume_vector vector = {
        {{"calendar", {"screens"}, {NULL}}},
        {{0, WATCH, "screens"}, {0, PROVIDE, "screens"}, {0, VERIFY, NULL}, {0, ALL, "screens"}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "[calendar]", "outcomes");
    check_text(answer.heard, "[calendar provide]", "heard");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void lets_a_released_plugin_read_all(void) {
    consume_vector vector = {
        {{"agenda", {NULL}, {"screens"}}, {"calendar", {"screens"}, {NULL}}},
        {{1, PROVIDE, "screens"}, {1, VERIFY, NULL}, {0, RELEASE, NULL}, {0, ALL, "screens"}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "[calendar]", "outcomes");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void refuses_a_late_watch_from_a_released_plugin(void) {
    consume_vector vector = {
        {{"agenda", {NULL}, {"screens"}}, {"calendar", {"screens"}, {NULL}}},
        {{0, RELEASE, NULL}, {0, WATCH, "screens"}, {1, PROVIDE, "screens"}, {1, VERIFY, NULL}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.heard, "[]", "heard");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void refuses_an_undeclared_all_and_reports_it_once(void) {
    consume_vector vector = {{{"agenda", {NULL}, {"screens"}},
                              {"calendar", {"screens"}, {NULL}},
                              {"others", {NULL}, {NULL}}},
                             {{1, PROVIDE, "screens"},
                              {1, VERIFY, NULL},
                              {2, ALL, "screens"},
                              {2, ALL, "screens"},
                              {0, ALL, "screens"}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "[],[],[calendar]", "outcomes");
    check_text(answer.reported, "others screens;", "reported");
    check_refusal_mentions(&answer, "screens");
}

static void refuses_an_undeclared_all_from_a_released_plugin(void) {
    consume_vector vector = {
        {{"calendar", {"screens"}, {NULL}}, {"others", {NULL}, {NULL}}},
        {{0, PROVIDE, "screens"}, {0, VERIFY, NULL}, {1, RELEASE, NULL}, {1, ALL, "screens"}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.outcomes, "[]", "outcomes");
    check_text(answer.reported, "others screens;", "reported");
    check_refusal_mentions(&answer, "screens");
}

static void refuses_an_undeclared_watch(void) {
    consume_vector vector = {{{"agenda", {NULL}, {"screens"}},
                              {"calendar", {"screens"}, {NULL}},
                              {"others", {NULL}, {NULL}}},
                             {{2, WATCH, "screens"},
                              {0, WATCH, "screens"},
                              {1, PROVIDE, "screens"},
                              {1, VERIFY, NULL}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.heard, "[],[calendar provide]", "heard");
    check_text(answer.reported, "others screens;", "reported");
    check_refusal_mentions(&answer, "screens");
}

static void watch_hears_a_quarantined_provider_withdrawn_once(void) {
    consume_vector vector = {{{"agenda", {NULL}, {"screens"}}, {"calendar", {"screens"}, {NULL}}},
                             {{0, WATCH, "screens"},
                              {1, PROVIDE, "screens"},
                              {1, VERIFY, NULL},
                              {1, QUARANTINE, NULL},
                              {1, RELEASE, NULL}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.heard, "[calendar provide, calendar withdraw]", "heard");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void watch_hears_a_released_provider_withdrawn(void) {
    consume_vector vector = {
        {{"calendar", {"screens"}, {NULL}}, {"agenda", {NULL}, {"screens"}}},
        {{1, WATCH, "screens"}, {0, PROVIDE, "screens"}, {0, VERIFY, NULL}, {0, RELEASE, NULL}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.heard, "[calendar provide, calendar withdraw]", "heard");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void watch_hears_a_verified_provider_and_not_a_refused_one(void) {
    consume_vector vector = {{{"agenda", {NULL}, {"screens"}},
                              {"calendar", {"screens"}, {NULL}},
                              {"flaky", {"screens", "extras"}, {NULL}}},
                             {{0, WATCH, "screens"},
                              {2, PROVIDE, "screens"},
                              {2, VERIFY, NULL},
                              {1, PROVIDE, "screens"},
                              {1, VERIFY, NULL}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.heard, "[calendar provide]", "heard");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static void watch_hears_nothing_from_before_it_was_installed(void) {
    consume_vector vector = {{{"agenda", {NULL}, {"screens"}},
                              {"calendar", {"screens"}, {NULL}},
                              {"notes", {"screens"}, {NULL}}},
                             {{1, PROVIDE, "screens"},
                              {1, VERIFY, NULL},
                              {0, WATCH, "screens"},
                              {2, PROVIDE, "screens"},
                              {2, VERIFY, NULL}}};
    consume_answer answer;
    run(&vector, &answer);
    check_text(answer.heard, "[notes provide]", "heard");
    check_text(answer.reported, "", "reported");
    check_text(answer.refusal, "", "refusal");
}

static const char *const SCREENS[] = {"screens", NULL};
static const char *const NONE[] = {NULL};

static weivel_handle provide(weivel_core *core, weivel_handle plugin, const char *id) {
    weivel_handle implementation = 0;
    CHECK(weivel_provide_capability(core, plugin, id, &implementation, NULL) == WEIVEL_STATUS_OK);
    CHECK(implementation != 0);
    return implementation;
}

static weivel_handle watch_capability(weivel_core *core, weivel_handle plugin, const char *id) {
    weivel_handle watch = 0;
    CHECK(weivel_watch_capability(core, plugin, id, &watch, NULL) == WEIVEL_STATUS_OK);
    CHECK(watch != 0);
    return watch;
}

static uint32_t capability_events(weivel_handle watch, uint32_t change) {
    uint32_t count = 0;
    for (uint32_t at = 0; at < the_journal.count; at++) {
        const seen *item = &the_journal.items[at];
        count += item->kind == WEIVEL_EVENT_CAPABILITY && item->subject == watch && item->change == change;
    }
    return count;
}

static uint32_t count_all(weivel_core *core, weivel_handle plugin, const char *id) {
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_capability_all(core, plugin, id, &list, NULL) == WEIVEL_STATUS_OK);
    uint32_t count = list.count;
    CHECK(weivel_capability_list_release(&list) == WEIVEL_STATUS_OK);
    return count;
}

static void the_refusal_texts_are_exact(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle others = open_capabilities(core, "others", NONE, NONE);
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_capability_all(core, others, "screens", &list, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    const seen *reach = nth_of(WEIVEL_EVENT_REACH_REFUSED, 0);
    CHECK(reach->plugin == others && reach->change == WEIVEL_REACH_CAPABILITY);
    check_text(reach->id, "screens", "reach id");
    check_text(reach->detail, "reached for capability \"screens\", which it does not declare", "undeclared detail");
    check_text(reach->fix, "add \"screens\" to the capabilities it consumes in its manifest", "undeclared fix");
    forget(core);

    const char *const two[] = {"screens", "extras", NULL};
    weivel_handle flaky = open_capabilities(core, "flaky", two, NONE);
    CHECK(weivel_verify_activation(core, flaky, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    const seen *missing = nth_of(WEIVEL_EVENT_REFUSAL, 0);
    CHECK(missing->code == WEIVEL_REFUSAL_ACTIVATION_MISMATCH);
    check_text(missing->detail, "capabilities declared but never provided: screens, extras", "missing detail");
    check_text(missing->fix,
               "provide \"screens\" while the plugin activates, or remove \"screens\" from the capabilities it "
               "provides in its manifest",
               "missing fix");
    forget(core);

    weivel_handle eager = open_capabilities(core, "eager", SCREENS, NONE);
    provide(core, eager, "screens");
    provide(core, eager, "widgets");
    provide(core, eager, "panels");
    weivel_handle again = 7;
    CHECK(weivel_provide_capability(core, eager, "screens", &again, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(again == 0);
    CHECK(weivel_verify_activation(core, eager, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    const seen *twice = nth_of(WEIVEL_EVENT_REFUSAL, 0);
    CHECK(twice->code == WEIVEL_REFUSAL_PROVIDED_TWICE);
    check_text(twice->detail, "provided capability \"screens\" twice", "twice detail");
    check_text(twice->fix, "provide each capability once while the plugin activates", "twice fix");
    const seen *extra = nth_of(WEIVEL_EVENT_REFUSAL, 1);
    CHECK(extra->code == WEIVEL_REFUSAL_ACTIVATION_MISMATCH);
    check_text(extra->detail, "capabilities provided but never declared: widgets, panels", "extra detail");
    check_text(extra->fix,
               "add \"widgets\" to the capabilities it provides in its manifest, or stop providing \"widgets\"",
               "extra fix");
    forget(core);

    CHECK(weivel_release_plugin(core, eager, NULL) == WEIVEL_STATUS_OK);
    weivel_handle late = 7;
    CHECK(weivel_provide_capability(core, eager, "screens", &late, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(late == 0);
    weivel_handle watch = 7;
    CHECK(weivel_watch_capability(core, eager, "screens", &watch, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(watch == 0);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REFUSAL) == 0 && count_kind(WEIVEL_EVENT_REACH_REFUSED) == 0);
    check_text(nth_of(WEIVEL_EVENT_LOG, 0)->detail,
               "ignored a late provision of capability \"screens\" from eager, which is no longer running",
               "late provision");
    check_text(nth_of(WEIVEL_EVENT_LOG, 1)->detail,
               "ignored a late watch of capability \"screens\" from eager, which is no longer running", "late watch");
    CHECK(nth_of(WEIVEL_EVENT_LOG, 1)->level == WEIVEL_LOG_WARNING);
    weivel_core_destroy(core);
}

/** A provider watching its own capability hears its implementation leave before its watch stops. */
static void a_self_watching_provider_hears_its_own_withdraw(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle calendar = open_capabilities(core, "calendar", SCREENS, NONE);
    weivel_handle watch = watch_capability(core, calendar, "screens");
    weivel_handle implementation = provide(core, calendar, "screens");
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_release_plugin(core, calendar, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_CAPABILITY) == 2);
    const seen *left = nth_of(WEIVEL_EVENT_CAPABILITY, 1);
    CHECK(left->subject == watch && left->change == WEIVEL_CAPABILITY_WITHDRAWN);
    CHECK(left->implementation == implementation && left->provider == calendar && left->plugin == calendar);
    forget(core);
    weivel_census census;
    weivel_core_census(core, &census);
    CHECK(census.capabilities == 0 && census.provisions == 0 && census.capability_watches == 0);
    CHECK(weivel_unregister(core, watch, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_STALE_HANDLE);
    weivel_core_destroy(core);
}

/** The declaration is checked before lateness, so a released plugin's undeclared watch is reported as undeclared. */
static void an_undeclared_watch_from_a_released_plugin_is_reported(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle others = open_capabilities(core, "others", NONE, NONE);
    CHECK(weivel_release_plugin(core, others, NULL) == WEIVEL_STATUS_OK);
    weivel_handle watch = 7;
    CHECK(weivel_watch_capability(core, others, "screens", &watch, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(watch == 0);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REACH_REFUSED) == 1);
    CHECK(count_kind(WEIVEL_EVENT_LOG) == 0);
    weivel_core_destroy(core);
}

static void a_watch_ends_by_its_handle_and_by_its_consumer_stopping(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle agenda = open_capabilities(core, "agenda", NONE, SCREENS);
    weivel_handle notes = open_capabilities(core, "notes", NONE, SCREENS);
    weivel_handle calendar = open_capabilities(core, "calendar", SCREENS, NONE);
    weivel_handle unwatched = watch_capability(core, agenda, "screens");
    weivel_handle quarantined = watch_capability(core, notes, "screens");
    CHECK(weivel_unregister(core, unwatched, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_unregister(core, unwatched, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_quarantine_plugin(core, notes, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, notes, NULL, NULL) == WEIVEL_STATUS_OK);
    forget(core);
    provide(core, calendar, "screens");
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_CAPABILITY) == 0);
    CHECK(capability_events(unwatched, WEIVEL_CAPABILITY_PROVIDED) == 0);
    CHECK(capability_events(quarantined, WEIVEL_CAPABILITY_PROVIDED) == 0);
    weivel_census census;
    weivel_core_census(core, &census);
    CHECK(census.capability_watches == 0 && census.provisions == 1 && census.capabilities == 1);
    weivel_core_destroy(core);
}

static void verifying_again_changes_nothing(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle agenda = open_capabilities(core, "agenda", NONE, SCREENS);
    weivel_handle calendar = open_capabilities(core, "calendar", SCREENS, NONE);
    weivel_handle watch = watch_capability(core, agenda, "screens");
    provide(core, calendar, "screens");
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(capability_events(watch, WEIVEL_CAPABILITY_PROVIDED) == 1);
    CHECK(count_all(core, agenda, "screens") == 1);
    weivel_core_destroy(core);
}

/** A quarantined provider reopened provides again, under a new handle, and rejoins only once verified again. */
static void a_reopened_provider_rejoins_once_verified_again(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle agenda = open_capabilities(core, "agenda", NONE, SCREENS);
    weivel_handle calendar = open_capabilities(core, "calendar", SCREENS, NONE);
    weivel_handle first = provide(core, calendar, "screens");
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_quarantine_plugin(core, calendar, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(count_all(core, agenda, "screens") == 0);
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_open_plugin(core, calendar, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_handle second = provide(core, calendar, "screens");
    CHECK(second != first);
    CHECK(count_all(core, agenda, "screens") == 0);
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_capability_all(core, agenda, "screens", &list, NULL) == WEIVEL_STATUS_OK);
    CHECK(list.count == 1 && list.items[0].implementation == second && list.items[0].provider == calendar);
    CHECK(weivel_capability_list_release(&list) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

static void a_service_and_a_capability_of_one_id_are_reported_apart(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle others = open_capabilities(core, "others", NONE, NONE);
    weivel_handle got = 0;
    CHECK(weivel_get_service(core, others, "screens", &got, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_capability_all(core, others, "screens", &list, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_handle watch = 0;
    CHECK(weivel_watch_capability(core, others, "screens", &watch, NULL) == WEIVEL_STATUS_REFUSED);
    drain(core);
    void *found = NULL;
    CHECK(weivel_handles_resolve(core->handles, others, WEIVEL_RECORD_PLUGIN, &found) == WEIVEL_STATUS_OK);
    const weivel_plugin *plugin = found;
    uint32_t services = 0;
    uint32_t capabilities = 0;
    for (uint32_t index = 0; index < weivel_reach_reported_count(plugin); index++) {
        const weivel_reach_record *record = weivel_reach_reported_at(plugin, index);
        services += record->kind == WEIVEL_REACH_SERVICE && strcmp(record->id, "screens") == 0;
        capabilities += record->kind == WEIVEL_REACH_CAPABILITY && strcmp(record->id, "screens") == 0;
    }
    CHECK(services == 1 && capabilities == 1);
    weivel_core_destroy(core);
}

static void malformed_and_refused_calls_are_values(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle agenda = open_capabilities(core, "agenda", NONE, SCREENS);
    weivel_handle unopened = register_capabilities(core, "unopened", SCREENS, SCREENS);
    weivel_handle implementation = 7;
    CHECK(weivel_provide_capability(NULL, agenda, "screens", &implementation, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_provide_capability(core, agenda, "", &implementation, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(implementation == 0);
    CHECK(weivel_provide_capability(core, agenda, "screens", NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_provide_capability(core, unopened, "screens", &implementation, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_verify_activation(NULL, agenda, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_verify_activation(core, unopened, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_verify_activation(core, 0, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_handle watch = 7;
    CHECK(weivel_watch_capability(core, agenda, NULL, &watch, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(watch == 0);
    CHECK(weivel_watch_capability(core, agenda, "screens", NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_watch_capability(core, unopened, "screens", &watch, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_capability_all(core, agenda, "", &list, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_capability_all(core, agenda, "screens", NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_capability_all(core, unopened, "screens", &list, NULL) == WEIVEL_STATUS_REFUSED);
    weivel_capability_list small = WEIVEL_INIT(weivel_capability_list);
    small.size = (uint32_t)WEIVEL_CAPABILITY_LIST_FIRST_SIZE - 1;
    CHECK(weivel_capability_all(core, agenda, "screens", &small, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_capability_list_release(&small) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_capability_list_release(NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_capability_list zero;
    memset(&zero, 0, sizeof zero);
    CHECK(weivel_capability_list_release(&zero) == WEIVEL_STATUS_OK);
    CHECK(weivel_capability_all(core, agenda, "screens", &list, NULL) == WEIVEL_STATUS_OK);
    CHECK(list.count == 0 && list.items == NULL && list.storage != NULL);
    CHECK(weivel_capability_all(core, agenda, "screens", &list, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_capability_list_release(&list) == WEIVEL_STATUS_OK);
    CHECK(list.storage == NULL && list.size == sizeof list);
    drain(core);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_MALFORMED_CALL);
    CHECK(count_kind(WEIVEL_EVENT_REACH_REFUSED) == 0);
    weivel_core_destroy(core);
}

static uint32_t holdings_now(weivel_core *core) {
    weivel_census census;
    weivel_core_census(core, &census);
    return census.capability_holdings;
}

/** A holder is told once when its provider is quarantined, and the release after it tells nobody again. */
static void a_holder_is_revoked_once_when_its_provider_is_quarantined(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle agenda = open_capabilities(core, "agenda", NONE, SCREENS);
    weivel_handle calendar = open_capabilities(core, "calendar", SCREENS, NONE);
    weivel_handle implementation = provide(core, calendar, "screens");
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);
    CHECK(count_all(core, agenda, "screens") == 1);
    CHECK(count_all(core, agenda, "screens") == 1);
    CHECK(holdings_now(core) == 1);
    forget(core);
    CHECK(weivel_quarantine_plugin(core, calendar, NULL, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_release_plugin(core, calendar, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REVOKED) == 1);
    CHECK(revocations_to(agenda, implementation) == 1);
    CHECK(holdings_now(core) == 0);
    weivel_core_destroy(core);
}

/** A plugin that holds and watches hears the revocation first, then the watch's withdrawal, each once. */
static void a_holder_that_watches_hears_both_when_its_provider_is_released(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle agenda = open_capabilities(core, "agenda", NONE, SCREENS);
    weivel_handle bystander = open_capabilities(core, "bystander", NONE, SCREENS);
    weivel_handle calendar = open_capabilities(core, "calendar", SCREENS, NONE);
    weivel_handle watch = watch_capability(core, agenda, "screens");
    weivel_handle implementation = provide(core, calendar, "screens");
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);
    CHECK(count_all(core, agenda, "screens") == 1);
    forget(core);
    CHECK(weivel_release_plugin(core, calendar, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(the_journal.count == 2);
    CHECK(the_journal.items[0].kind == WEIVEL_EVENT_REVOKED);
    CHECK(revocations_to(agenda, implementation) == 1);
    CHECK(revocations_to(bystander, implementation) == 0);
    CHECK(the_journal.items[1].kind == WEIVEL_EVENT_CAPABILITY);
    CHECK(capability_events(watch, WEIVEL_CAPABILITY_WITHDRAWN) == 1);
    weivel_core_destroy(core);
}

/**
 * A released holder has what it holds revoked to it as it is released, and hears nothing when the provider goes
 * later. Its late `all` holds again, revoked to it when it is forgotten.
 */
static void a_released_holder_hears_nothing_later(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle agenda = open_capabilities(core, "agenda", NONE, SCREENS);
    weivel_handle notes = open_capabilities(core, "notes", NONE, SCREENS);
    weivel_handle calendar = open_capabilities(core, "calendar", SCREENS, NONE);
    weivel_handle implementation = provide(core, calendar, "screens");
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);
    CHECK(count_all(core, agenda, "screens") == 1);
    CHECK(count_all(core, notes, "screens") == 1);
    forget(core);
    CHECK(weivel_release_plugin(core, agenda, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REVOKED) == 1 && revocations_to(agenda, implementation) == 1);
    CHECK(count_all(core, agenda, "screens") == 1);
    CHECK(weivel_forget_plugin(core, agenda, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(agenda, implementation) == 2);
    forget(core);
    CHECK(weivel_release_plugin(core, calendar, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REVOKED) == 1 && revocations_to(notes, implementation) == 1);
    CHECK(holdings_now(core) == 0);
    weivel_core_destroy(core);
}

/** A holding the core cannot record hands nothing over, so no revocation is ever owed for it. */
static void an_all_that_cannot_record_its_holding_hands_nothing_over(void) {
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle agenda = open_capabilities(core, "agenda", NONE, SCREENS);
    weivel_handle calendar = open_capabilities(core, "calendar", SCREENS, NONE);
    provide(core, calendar, "screens");
    CHECK(weivel_verify_activation(core, calendar, NULL) == WEIVEL_STATUS_OK);
    core->registry->fail_next_holding = true;
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_capability_all(core, agenda, "screens", &list, NULL) == WEIVEL_STATUS_OUT_OF_MEMORY);
    CHECK(list.count == 0 && list.storage == NULL);
    CHECK(holdings_now(core) == 0);
    forget(core);
    CHECK(weivel_release_plugin(core, calendar, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REVOKED) == 0);
    weivel_core_destroy(core);
}

static void churn_round(weivel_core *core, weivel_handle consumer) {
    weivel_handle provider = open_capabilities(core, "churn", SCREENS, SCREENS);
    provide(core, provider, "screens");
    watch_capability(core, provider, "screens");
    CHECK(weivel_verify_activation(core, provider, NULL) == WEIVEL_STATUS_OK);
    CHECK(count_all(core, consumer, "screens") == 1);
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_capability_all(core, provider, "undeclared", &list, NULL) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_release_plugin(core, provider, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_forget_plugin(core, provider, NULL) == WEIVEL_STATUS_OK);
    forget(core);
}

static bool same_capability_counts(const weivel_census *left, const weivel_census *right) {
    return left->plugins == right->plugins && left->capabilities == right->capabilities &&
           left->provisions == right->provisions && left->capability_watches == right->capability_watches &&
           left->capability_holdings == right->capability_holdings && left->live_handles == right->live_handles;
}

/** Providers come and go thousands of times under one consumer's watch, and leave the core no larger. */
static void churn_leaves_no_capability_behind(void) {
    enum { ROUNDS = 3000 };
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle consumer = open_capabilities(core, "consumer", NONE, SCREENS);
    watch_capability(core, consumer, "screens");
    weivel_census before;
    weivel_core_census(core, &before);
    churn_round(core, consumer);
    weivel_census after_one;
    weivel_core_census(core, &after_one);
    CHECK(same_capability_counts(&before, &after_one));
    uint32_t drifted = 0;
    for (uint32_t round = 1; round < ROUNDS; round++) {
        churn_round(core, consumer);
        weivel_census now;
        weivel_core_census(core, &now);
        drifted += !same_capability_counts(&before, &now) || now.handle_slots != after_one.handle_slots;
    }
    CHECK(drifted == 0);
    CHECK(before.capabilities == 1 && before.provisions == 0 && before.capability_watches == 1);
    CHECK(before.capability_holdings == 0);
    weivel_core_destroy(core);
}

/** Thousands of providers of one capability answer in verify order, and withdrawing half keeps the rest in it. */
static void many_providers_keep_verify_order(void) {
    enum { PROVIDERS = 3000 };
    weivel_core *core = core_knowing(NULL, 0);
    weivel_handle reader = open_capabilities(core, "reader", NONE, SCREENS);
    weivel_handle *providers = malloc(PROVIDERS * sizeof *providers);
    weivel_handle *implementations = malloc(PROVIDERS * sizeof *implementations);
    CHECK(providers != NULL && implementations != NULL);
    char name[32];
    for (uint32_t index = 0; providers != NULL && implementations != NULL && index < PROVIDERS; index++) {
        snprintf(name, sizeof name, "provider%u", (unsigned)index);
        providers[index] = open_capabilities(core, name, SCREENS, NONE);
        implementations[index] = provide(core, providers[index], "screens");
    }
    for (uint32_t index = PROVIDERS; providers != NULL && index-- > 0;) {
        CHECK(weivel_verify_activation(core, providers[index], NULL) == WEIVEL_STATUS_OK);
    }
    for (uint32_t index = 0; providers != NULL && index < PROVIDERS; index += 2) {
        CHECK(weivel_release_plugin(core, providers[index], NULL) == WEIVEL_STATUS_OK);
    }
    forget(core);
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_capability_all(core, reader, "screens", &list, NULL) == WEIVEL_STATUS_OK);
    CHECK(list.count == PROVIDERS / 2);
    uint32_t out_of_order = 0;
    for (uint32_t index = 0; implementations != NULL && index < list.count; index++) {
        uint32_t expected = PROVIDERS - 1 - 2 * index;
        out_of_order += list.items[index].implementation != implementations[expected] ||
                        list.items[index].provider != providers[expected];
    }
    CHECK(out_of_order == 0);
    CHECK(weivel_capability_list_release(&list) == WEIVEL_STATUS_OK);
    free(providers);
    free(implementations);
    weivel_core_destroy(core);
}

int main(void) {
    a_released_consumer_stops_hearing();
    a_throwing_listener_does_not_silence_the_others();
    answers_every_verified_implementation_in_activation_order();
    answers_in_the_order_activations_were_verified();
    does_not_answer_a_provider_whose_activation_was_refused();
    does_not_answer_an_implementation_before_its_activation_is_verified();
    drops_a_released_provider_from_all();
    lets_a_plugin_reach_a_capability_it_provides();
    lets_a_released_plugin_read_all();
    refuses_a_late_watch_from_a_released_plugin();
    refuses_an_undeclared_all_and_reports_it_once();
    refuses_an_undeclared_all_from_a_released_plugin();
    refuses_an_undeclared_watch();
    watch_hears_a_quarantined_provider_withdrawn_once();
    watch_hears_a_released_provider_withdrawn();
    watch_hears_a_verified_provider_and_not_a_refused_one();
    watch_hears_nothing_from_before_it_was_installed();
    the_refusal_texts_are_exact();
    a_self_watching_provider_hears_its_own_withdraw();
    an_undeclared_watch_from_a_released_plugin_is_reported();
    a_watch_ends_by_its_handle_and_by_its_consumer_stopping();
    verifying_again_changes_nothing();
    a_reopened_provider_rejoins_once_verified_again();
    a_service_and_a_capability_of_one_id_are_reported_apart();
    malformed_and_refused_calls_are_values();
    a_holder_is_revoked_once_when_its_provider_is_quarantined();
    a_holder_that_watches_hears_both_when_its_provider_is_released();
    a_released_holder_hears_nothing_later();
    an_all_that_cannot_record_its_holding_hands_nothing_over();
    churn_leaves_no_capability_behind();
    many_providers_keep_verify_order();
    return check_summary("capabilities_test");
}
