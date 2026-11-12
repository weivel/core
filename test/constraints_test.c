#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <weivel/core.h>

#include "core_internal.h"
#include "registry_support.h"

#define SERVICE "storage.object"
#define MAX_NUMBERS 4
#define MAX_POLLS 4
#define MAX_ASKS 16
#define TICK_LIMIT 200

static const char *const well_known[] = {SERVICE};

static weivel_constraint_entry entry_of(const char *constraint_name, const char *attribute_name, uint32_t bound) {
    weivel_constraint_entry entry = WEIVEL_INIT(weivel_constraint_entry);
    entry.constraint_name = constraint_name;
    entry.attribute_name = attribute_name;
    entry.bound = bound;
    return entry;
}

/** The vocabulary the kit's drivers declare, since the core mints none. */
static weivel_core *core_with_vocabulary(void) {
    weivel_constraint_entry entries[2] = {entry_of("minReplicas", "maxReplicas", WEIVEL_BOUND_LOWER),
                                          entry_of("maxLatencyMs", "latencyMs", WEIVEL_BOUND_UPPER)};
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.well_known_services.items = well_known;
    options.well_known_services.count = 1;
    options.constraint_vocabulary.items = entries;
    options.constraint_vocabulary.count = 2;
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    the_journal.count = 0;
    return core;
}

typedef struct numbers_spec {
    const char *names[MAX_NUMBERS];
    double values[MAX_NUMBERS];
} numbers_spec;

static uint32_t numbers_count(const numbers_spec *spec) {
    return count_of(spec->names, MAX_NUMBERS);
}

static weivel_numbers numbers_from(const numbers_spec *spec, weivel_number *storage) {
    uint32_t count = numbers_count(spec);
    for (uint32_t index = 0; index < count; index++) {
        weivel_number number = WEIVEL_INIT(weivel_number);
        number.name = spec->names[index];
        number.value = spec->values[index];
        storage[index] = number;
    }
    weivel_numbers numbers = {count == 0 ? NULL : storage, count};
    return numbers;
}

static weivel_handle open_provider(weivel_core *core, const char *identity, const numbers_spec *advertised) {
    weivel_number storage[MAX_NUMBERS];
    weivel_service_offer offer = WEIVEL_INIT(weivel_service_offer);
    offer.id = SERVICE;
    offer.attributes = numbers_from(advertised, storage);
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = identity;
    manifest.api = 1;
    manifest.services.provides.items = &offer;
    manifest.services.provides.count = 1;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static weivel_handle open_consumer(weivel_core *core, const numbers_spec *constraints, bool degradable,
                                   const char *provider) {
    weivel_number storage[MAX_NUMBERS];
    weivel_service_want want = WEIVEL_INIT(weivel_service_want);
    want.id = SERVICE;
    want.provider = provider;
    want.optional = WEIVEL_CHOICE_NO;
    want.degradable = degradable ? WEIVEL_CHOICE_YES : WEIVEL_CHOICE_NO;
    want.constraints = numbers_from(constraints, storage);
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "consumer";
    manifest.api = 1;
    manifest.services.consumes.items = &want;
    manifest.services.consumes.count = 1;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static weivel_handle want_budgeted(weivel_core *core, weivel_handle plugin, int64_t now, int64_t budget,
                                   int64_t poll) {
    weivel_want_options options = WEIVEL_INIT(weivel_want_options);
    options.now = now;
    options.budgeted = WEIVEL_CHOICE_YES;
    options.budget_millis = budget;
    options.poll_millis = poll;
    weivel_handle want = 0;
    CHECK(weivel_want_service(core, plugin, SERVICE, &options, &want, NULL) == WEIVEL_STATUS_OK);
    CHECK(want != 0);
    return want;
}

static weivel_status report_attributes(weivel_core *core, const seen *asked, const numbers_spec *attributes,
                                       int64_t now) {
    weivel_number storage[MAX_NUMBERS];
    weivel_capacity capacity = WEIVEL_INIT(weivel_capacity);
    capacity.request = asked->request;
    capacity.attributes = numbers_from(attributes, storage);
    return weivel_report_capacity(core, asked->subject, asked->implementation, &capacity, now, NULL);
}

static weivel_status report_serves(weivel_core *core, const seen *asked, bool serves, int64_t now) {
    weivel_capacity capacity = WEIVEL_INIT(weivel_capacity);
    capacity.request = asked->request;
    capacity.serves = serves ? WEIVEL_CHOICE_YES : WEIVEL_CHOICE_NO;
    return weivel_report_capacity(core, asked->subject, asked->implementation, &capacity, now, NULL);
}

static uint32_t questions_of(uint32_t question) {
    uint32_t count = 0;
    for (uint32_t at = 0; at < the_journal.count; at++) {
        const seen *item = &the_journal.items[at];
        count += item->kind == WEIVEL_EVENT_CAPACITY_WANTED && item->question == question;
    }
    return count;
}

/** The latest capacity question in the journal, or an empty entry. */
static const seen *latest_question(void) {
    for (uint32_t at = the_journal.count; at > 0; at--) {
        if (the_journal.items[at - 1].kind == WEIVEL_EVENT_CAPACITY_WANTED) {
            return &the_journal.items[at - 1];
        }
    }
    return &no_event_seen;
}

static void advance_to(weivel_core *core, int64_t now) {
    CHECK(weivel_advance(core, now, NULL) == WEIVEL_STATUS_OK);
    drain(core);
}

/** A provider as a resolve vector scripts it, recording what it was asked. */
typedef struct scripted {
    numbers_spec advertised;
    numbers_spec by_poll[MAX_POLLS];
    bool serves_only_unconstrained;
    uint32_t reads;
    uint32_t reads_at_service;
    bool asks[MAX_ASKS];
    uint32_t ask_count;
} scripted;

typedef struct resolve_given {
    numbers_spec constraints;
    bool degradable;
    scripted provider;
    int64_t budget;
    int64_t poll;
} resolve_given;

typedef struct resolve_outcome {
    bool settled;
    bool served;
    bool degraded;
    uint32_t at_poll;
    int64_t settled_at;
    uint32_t code;
    char report[512];
    char detail[512];
    char fix[512];
} resolve_outcome;

/** The attributes a scripted provider reports at its latest read, clamped at the script's last entry. */
static const numbers_spec *scripted_current(const scripted *provider) {
    uint32_t polls = 0;
    while (polls < MAX_POLLS && provider->by_poll[polls].names[0] != NULL) {
        polls++;
    }
    static const numbers_spec nothing_live = {{NULL}, {0}};
    if (polls == 0) {
        return &nothing_live;
    }
    uint32_t index = provider->reads < 1 ? 1 : provider->reads;
    return &provider->by_poll[(index > polls ? polls : index) - 1];
}

static void answer(weivel_core *core, scripted *provider, const seen *asked, int64_t now) {
    if (asked->question == WEIVEL_CAPACITY_ATTRIBUTES) {
        provider->reads++;
        CHECK(report_attributes(core, asked, scripted_current(provider), now) == WEIVEL_STATUS_OK);
        return;
    }
    bool constrained = asked->constraint_count > 0;
    if (provider->ask_count < MAX_ASKS) {
        provider->asks[provider->ask_count++] = constrained;
    }
    bool serves = !(provider->serves_only_unconstrained && constrained);
    if (serves) {
        provider->reads_at_service = provider->reads;
    }
    CHECK(report_serves(core, asked, serves, now) == WEIVEL_STATUS_OK);
}

/** Answers every question and reads every settlement the journal holds from `processed` on, as they arrive. */
static void react(weivel_core *core, scripted *provider, weivel_handle want, int64_t now, uint32_t *processed,
                  resolve_outcome *outcome) {
    drain(core);
    while (*processed < the_journal.count) {
        const seen item = the_journal.items[(*processed)++];
        if (item.kind == WEIVEL_EVENT_CAPACITY_WANTED && item.subject == want) {
            answer(core, provider, &item, now);
            drain(core);
        } else if (item.kind == WEIVEL_EVENT_LOG && strstr(item.detail, "accepted a degraded") != NULL) {
            copy_text(outcome->report, sizeof outcome->report, item.detail);
        } else if (item.kind == WEIVEL_EVENT_WANT_SETTLED && item.subject == want) {
            outcome->settled = true;
            outcome->settled_at = now;
            outcome->degraded = item.degraded != 0;
            outcome->served = item.implementation != 0 && !outcome->degraded;
            outcome->code = item.code;
            copy_text(outcome->detail, sizeof outcome->detail, item.detail);
            copy_text(outcome->fix, sizeof outcome->fix, item.fix);
        }
    }
}

/** The kit's `resolve` op against the core: one provider, one consumer, the clock advanced deadline by deadline. */
static resolve_outcome resolve(resolve_given *given) {
    resolve_outcome outcome;
    memset(&outcome, 0, sizeof outcome);
    weivel_core *core = core_with_vocabulary();
    weivel_handle provider = open_provider(core, "the-provider", &given->provider.advertised);
    weivel_handle consumer = open_consumer(core, &given->constraints, given->degradable, NULL);
    register_service(core, provider, SERVICE);
    forget(core);
    weivel_handle want = want_budgeted(core, consumer, 0, given->budget, given->poll);
    uint32_t processed = 0;
    int64_t now = 0;
    react(core, &given->provider, want, now, &processed, &outcome);
    for (uint32_t tick = 0; tick < TICK_LIMIT && !outcome.settled; tick++) {
        if (weivel_next_deadline(core, &now, NULL) != WEIVEL_STATUS_OK) {
            break;
        }
        CHECK(weivel_advance(core, now, NULL) == WEIVEL_STATUS_OK);
        react(core, &given->provider, want, now, &processed, &outcome);
    }
    CHECK(outcome.settled);
    int64_t left;
    CHECK(weivel_next_deadline(core, &left, NULL) == WEIVEL_STATUS_NONE);
    outcome.at_poll = given->provider.reads_at_service;
    weivel_core_destroy(core);
    return outcome;
}

static void a_live_attribute_leaves_the_other_advertised_claims_standing(void) {
    resolve_given given = {{{"minReplicas", "maxLatencyMs"}, {3, 50}},
                           false,
                           {.advertised = {{"maxReplicas", "latencyMs"}, {3, 10}}, .by_poll = {{{"maxReplicas"}, {5}}}},
                           2000,
                           1000};
    resolve_outcome outcome = resolve(&given);
    CHECK(outcome.served);
    CHECK(outcome.at_poll == 1);
    CHECK(!outcome.degraded);
    CHECK(outcome.code == WEIVEL_REFUSAL_NONE);
    CHECK(outcome.settled_at == 0);
}

static void an_empty_live_attribute_map_keeps_the_advertised_claim(void) {
    resolve_given given = {{{"minReplicas"}, {3}},
                           false,
                           {.advertised = {{"maxReplicas"}, {3}}, .by_poll = {{{NULL}, {0}}}},
                           30000,
                           1000};
    resolve_outcome outcome = resolve(&given);
    CHECK(outcome.served);
    CHECK(outcome.at_poll == 1);
    CHECK(!outcome.degraded);
    CHECK(outcome.code == WEIVEL_REFUSAL_NONE);
}

static void asks_with_the_consumers_constraints_before_falling_back(void) {
    resolve_given given = {{{"minReplicas"}, {3}},
                           true,
                           {.advertised = {{"maxReplicas"}, {1}}, .by_poll = {{{"maxReplicas"}, {1}}}},
                           2000,
                           1000};
    given.provider.serves_only_unconstrained = true;
    resolve_outcome outcome = resolve(&given);
    CHECK(given.provider.ask_count == 2);
    CHECK(given.provider.asks[0] == true);
    CHECK(given.provider.asks[1] == false);
    CHECK(outcome.degraded);
    CHECK(!outcome.served);
    CHECK(outcome.code == WEIVEL_REFUSAL_NONE);
}

static void degrades_when_never_satisfied_and_degradable(void) {
    resolve_given given = {{{"minReplicas"}, {3}},
                           true,
                           {.advertised = {{"maxReplicas"}, {1}}, .by_poll = {{{"maxReplicas"}, {1}}}},
                           2000,
                           1000};
    resolve_outcome outcome = resolve(&given);
    CHECK(!outcome.served);
    CHECK(outcome.degraded);
    mentions(outcome.report, "minReplicas");
    mentions(outcome.report, "maxReplicas");
    check_text(outcome.report,
               "consumer accepted a degraded \"storage.object\": it asked for minReplicas 3 and was offered "
               "maxReplicas 1",
               "degraded report");
    CHECK(outcome.code == WEIVEL_REFUSAL_NONE);
    CHECK(outcome.settled_at == 2000);
    CHECK(given.provider.reads == 3);
}

static void refuses_when_never_satisfied_and_not_degradable(void) {
    resolve_given given = {{{"minReplicas"}, {3}},
                           false,
                           {.advertised = {{"maxReplicas"}, {1}}, .by_poll = {{{"maxReplicas"}, {1}}}},
                           2000,
                           1000};
    resolve_outcome outcome = resolve(&given);
    CHECK(!outcome.served);
    CHECK(!outcome.degraded);
    CHECK(outcome.code == WEIVEL_REFUSAL_UNMET_CONSTRAINT);
    mentions(outcome.detail, "minReplicas");
    mentions(outcome.detail, "maxReplicas");
    mentions(outcome.detail, "2000");
    check_text(outcome.detail,
               "needs \"storage.object\" meeting minReplicas 3, and after 2000 ms its provider still offers "
               "maxReplicas 1",
               "unmet detail");
    check_text(outcome.fix,
               "run a provider that meets minReplicas 3, or mark the requirement degradable to accept what there is",
               "unmet fix");
    CHECK(outcome.settled_at == 2000);
}

static void serves_when_attributes_satisfy_before_the_deadline(void) {
    resolve_given given = {{{"minReplicas"}, {3}},
                           false,
                           {.advertised = {{"maxReplicas"}, {1}},
                            .by_poll = {{{"maxReplicas"}, {1}}, {{"maxReplicas"}, {3}}}},
                           30000,
                           1000};
    resolve_outcome outcome = resolve(&given);
    CHECK(outcome.served);
    CHECK(outcome.at_poll == 2);
    CHECK(!outcome.degraded);
    CHECK(outcome.code == WEIVEL_REFUSAL_NONE);
    CHECK(outcome.settled_at == 1000);
}

/** One provider and one consumer, the want made at 0 under a 2000 ms budget polled every 1000 ms. */
typedef struct scene {
    weivel_core *core;
    weivel_handle provider;
    weivel_handle consumer;
    weivel_handle registration;
    weivel_handle want;
} scene;

static scene scene_of(bool degradable, double advertised_replicas) {
    scene made;
    made.core = core_with_vocabulary();
    numbers_spec advertised = {{"maxReplicas"}, {advertised_replicas}};
    numbers_spec constraints = {{"minReplicas"}, {3}};
    made.provider = open_provider(made.core, "the-provider", &advertised);
    made.consumer = open_consumer(made.core, &constraints, degradable, NULL);
    made.registration = register_service(made.core, made.provider, SERVICE);
    forget(made.core);
    made.want = want_budgeted(made.core, made.consumer, 0, 2000, 1000);
    drain(made.core);
    return made;
}

static const numbers_spec three_replicas = {{"maxReplicas"}, {3}};
static const numbers_spec one_replica = {{"maxReplicas"}, {1}};

/** A capacity reported at one poll is never carried to the next: every poll asks for the attributes afresh. */
static void every_poll_asks_for_the_attributes_afresh(void) {
    scene made = scene_of(false, 1);
    seen first = *latest_question();
    CHECK(first.question == WEIVEL_CAPACITY_ATTRIBUTES);
    CHECK(report_attributes(made.core, &first, &three_replicas, 0) == WEIVEL_STATUS_OK);
    drain(made.core);
    seen serve_ask = *latest_question();
    CHECK(serve_ask.question == WEIVEL_CAPACITY_SERVE);
    CHECK(report_serves(made.core, &serve_ask, false, 0) == WEIVEL_STATUS_OK);
    advance_to(made.core, 1000);
    seen second = *latest_question();
    CHECK(second.question == WEIVEL_CAPACITY_ATTRIBUTES);
    CHECK(second.request != first.request && second.request != serve_ask.request);
    CHECK(questions_of(WEIVEL_CAPACITY_SERVE) == 1);
    CHECK(settlement_of(made.want) == NULL);
    weivel_core_destroy(made.core);
}

/** A poll the provider leaves unanswered is "not right now", decided on nothing an earlier poll reported. */
static void a_poll_left_unanswered_is_not_right_now(void) {
    scene made = scene_of(false, 1);
    seen first = *latest_question();
    CHECK(report_attributes(made.core, &first, &three_replicas, 0) == WEIVEL_STATUS_OK);
    drain(made.core);
    CHECK(report_serves(made.core, latest_question(), false, 0) == WEIVEL_STATUS_OK);
    advance_to(made.core, 1000);
    uint32_t serves_before = questions_of(WEIVEL_CAPACITY_SERVE);
    advance_to(made.core, 2000);
    CHECK(questions_of(WEIVEL_CAPACITY_SERVE) == serves_before);
    CHECK(settlement_of(made.want) == NULL);
    seen last = *latest_question();
    CHECK(last.question == WEIVEL_CAPACITY_ATTRIBUTES);
    advance_to(made.core, 3000);
    const seen *settled = settlement_of(made.want);
    CHECK(settled != NULL && settled->code == WEIVEL_REFUSAL_UNMET_CONSTRAINT && settled->implementation == 0);
    if (settled != NULL) {
        mentions(settled->detail, "maxReplicas 3");
    }
    CHECK(questions_of(WEIVEL_CAPACITY_SERVE) == serves_before);
    weivel_core_destroy(made.core);
}

/** An answer to an earlier poll's question counts for nothing, even with the want still polling. */
static void an_answer_from_an_earlier_poll_counts_for_nothing(void) {
    scene made = scene_of(false, 1);
    seen first = *latest_question();
    advance_to(made.core, 1000);
    seen second = *latest_question();
    CHECK(second.request != first.request);
    uint32_t before = the_journal.count;
    CHECK(report_attributes(made.core, &first, &three_replicas, 1000) == WEIVEL_STATUS_NONE);
    drain(made.core);
    CHECK(the_journal.count == before);
    CHECK(report_attributes(made.core, &second, &one_replica, 1000) == WEIVEL_STATUS_OK);
    drain(made.core);
    CHECK(questions_of(WEIVEL_CAPACITY_SERVE) == 0);
    CHECK(report_attributes(made.core, &second, &three_replicas, 1000) == WEIVEL_STATUS_NONE);
    weivel_core_destroy(made.core);
}

/** A poll ends when the next one starts, whether or not the caller has advanced the core yet. */
static void an_answer_after_its_poll_ended_counts_for_nothing(void) {
    scene made = scene_of(false, 1);
    seen first = *latest_question();
    CHECK(report_attributes(made.core, &first, &three_replicas, 1000) == WEIVEL_STATUS_NONE);
    CHECK(report_attributes(made.core, &first, &three_replicas, 999) == WEIVEL_STATUS_OK);
    weivel_core_destroy(made.core);
}

/** A degradable consumer never settles before its budget is spent, however plainly the provider falls short. */
static void a_degradable_want_waits_out_its_budget(void) {
    scene made = scene_of(true, 1);
    CHECK(report_attributes(made.core, latest_question(), &one_replica, 0) == WEIVEL_STATUS_OK);
    drain(made.core);
    CHECK(settlement_of(made.want) == NULL);
    CHECK(questions_of(WEIVEL_CAPACITY_SERVE) == 0);
    advance_to(made.core, 1000);
    CHECK(report_attributes(made.core, latest_question(), &one_replica, 1000) == WEIVEL_STATUS_OK);
    drain(made.core);
    CHECK(settlement_of(made.want) == NULL);
    CHECK(questions_of(WEIVEL_CAPACITY_SERVE) == 0);
    advance_to(made.core, 2000);
    CHECK(report_attributes(made.core, latest_question(), &one_replica, 2000) == WEIVEL_STATUS_OK);
    drain(made.core);
    seen degrade_ask = *latest_question();
    CHECK(degrade_ask.question == WEIVEL_CAPACITY_SERVE && degrade_ask.constraint_count == 1);
    CHECK(report_serves(made.core, &degrade_ask, true, 2000) == WEIVEL_STATUS_OK);
    drain(made.core);
    const seen *settled = settlement_of(made.want);
    CHECK(settled != NULL && settled->degraded == 1 && settled->implementation == made.registration);
    weivel_core_destroy(made.core);
}

/** With nothing registered, a degradable consumer is asked nothing and refused once its budget is spent. */
static void a_degradable_want_never_accepts_an_absent_provider(void) {
    weivel_core *core = core_with_vocabulary();
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle consumer = open_consumer(core, &constraints, true, NULL);
    forget(core);
    weivel_handle want = want_budgeted(core, consumer, 0, 2000, 1000);
    drain(core);
    advance_to(core, 1000);
    CHECK(settlement_of(want) == NULL);
    advance_to(core, 2000);
    CHECK(questions_of(WEIVEL_CAPACITY_ATTRIBUTES) == 0 && questions_of(WEIVEL_CAPACITY_SERVE) == 0);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->code == WEIVEL_REFUSAL_UNMET_CONSTRAINT && settled->degraded == 0);
    if (settled != NULL) {
        check_text(settled->detail,
                   "needs \"storage.object\" meeting minReplicas 3, and after 2000 ms nothing provides it",
                   "absent detail");
    }
    weivel_core_destroy(core);
}

/** A provider the consumer is not bound to is absent to it, so it is neither polled nor degraded to. */
static void a_degradable_want_never_accepts_a_provider_it_is_not_bound_to(void) {
    weivel_core *core = core_with_vocabulary();
    numbers_spec advertised = {{"maxReplicas"}, {1}};
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle consumer = open_consumer(core, &constraints, true, "forge:1");
    forget(core);
    weivel_handle want = want_budgeted(core, consumer, 0, 2000, 1000);
    weivel_handle other = open_provider(core, "other", &advertised);
    register_service(core, other, SERVICE);
    drain(core);
    advance_to(core, 1000);
    advance_to(core, 2000);
    CHECK(questions_of(WEIVEL_CAPACITY_ATTRIBUTES) == 0 && questions_of(WEIVEL_CAPACITY_SERVE) == 0);
    CHECK(count_kind(WEIVEL_EVENT_REACH_REFUSED) == 1);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->code == WEIVEL_REFUSAL_UNMET_CONSTRAINT && settled->implementation == 0);
    weivel_core_destroy(core);
}

/** A degradable want whose provider will serve no ask at all is refused, saying so. */
static void a_provider_that_serves_nothing_is_refused(void) {
    scene made = scene_of(true, 1);
    advance_to(made.core, 1000);
    advance_to(made.core, 2000);
    advance_to(made.core, 3000);
    seen constrained_ask = *latest_question();
    CHECK(constrained_ask.question == WEIVEL_CAPACITY_SERVE && constrained_ask.constraint_count == 1);
    CHECK(report_serves(made.core, &constrained_ask, false, 3000) == WEIVEL_STATUS_OK);
    drain(made.core);
    seen unconstrained_ask = *latest_question();
    CHECK(unconstrained_ask.question == WEIVEL_CAPACITY_SERVE && unconstrained_ask.constraint_count == 0);
    advance_to(made.core, 4000);
    const seen *settled = settlement_of(made.want);
    CHECK(settled != NULL && settled->code == WEIVEL_REFUSAL_NOT_SERVED);
    if (settled != NULL) {
        check_text(settled->detail,
                   "needs \"storage.object\", and its provider would not serve even an unconstrained ask",
                   "not served detail");
        check_text(settled->fix,
                   "have the provider serve an unconstrained ask, or stop declaring the requirement degradable",
                   "not served fix");
    }
    int64_t left;
    CHECK(weivel_next_deadline(made.core, &left, NULL) == WEIVEL_STATUS_NONE);
    weivel_core_destroy(made.core);
}

/** Satisfying attributes at the last poll with a refusal to serve still ask under the constraints once more. */
static void the_last_poll_refused_still_asks_once_more_before_falling_back(void) {
    scene made = scene_of(true, 1);
    advance_to(made.core, 1000);
    advance_to(made.core, 2000);
    CHECK(report_attributes(made.core, latest_question(), &three_replicas, 2000) == WEIVEL_STATUS_OK);
    drain(made.core);
    CHECK(report_serves(made.core, latest_question(), false, 2000) == WEIVEL_STATUS_OK);
    drain(made.core);
    seen again = *latest_question();
    CHECK(again.question == WEIVEL_CAPACITY_SERVE && again.constraint_count == 1);
    CHECK(report_serves(made.core, &again, false, 2000) == WEIVEL_STATUS_OK);
    drain(made.core);
    seen fallback = *latest_question();
    CHECK(fallback.question == WEIVEL_CAPACITY_SERVE && fallback.constraint_count == 0);
    CHECK(report_serves(made.core, &fallback, true, 2000) == WEIVEL_STATUS_OK);
    drain(made.core);
    CHECK(questions_of(WEIVEL_CAPACITY_SERVE) == 3);
    const seen *settled = settlement_of(made.want);
    CHECK(settled != NULL && settled->degraded == 1);
    const seen *report = nth_of(WEIVEL_EVENT_LOG, 0);
    mentions(report->detail, "was offered maxReplicas 3");
    weivel_core_destroy(made.core);
}

/** An upper bound is met at or below its value and missed above it. */
static void an_upper_bound_is_met_only_at_or_below_its_value(void) {
    resolve_given within = {{{"maxLatencyMs"}, {50}},
                            false,
                            {.advertised = {{"latencyMs"}, {50}}, .by_poll = {{{NULL}, {0}}}},
                            0,
                            1000};
    CHECK(resolve(&within).served);
    resolve_given above = {{{"maxLatencyMs"}, {50}},
                           false,
                           {.advertised = {{"latencyMs"}, {51}}, .by_poll = {{{NULL}, {0}}}},
                           0,
                           1000};
    resolve_outcome refused = resolve(&above);
    CHECK(!refused.served && refused.code == WEIVEL_REFUSAL_UNMET_CONSTRAINT);
    resolve_given lower = {{{"minReplicas"}, {3}},
                           false,
                           {.advertised = {{"maxReplicas"}, {4}}, .by_poll = {{{NULL}, {0}}}},
                           0,
                           1000};
    CHECK(resolve(&lower).served);
}

/** A constraint the vocabulary does not name is logged once and never met. */
static void a_constraint_no_vocabulary_names_is_never_met(void) {
    weivel_core *core = core_with_vocabulary();
    numbers_spec advertised = {{"gpus"}, {8}};
    numbers_spec constraints = {{"minGpus"}, {1}};
    weivel_handle provider = open_provider(core, "the-provider", &advertised);
    weivel_handle consumer = open_consumer(core, &constraints, false, NULL);
    register_service(core, provider, SERVICE);
    forget(core);
    weivel_handle want = want_budgeted(core, consumer, 0, 1000, 1000);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_LOG) == 1);
    check_text(nth_of(WEIVEL_EVENT_LOG, 0)->detail,
               "constraint \"minGpus\" is in no vocabulary this host declared, so nothing can satisfy it for consumer",
               "unknown constraint");
    numbers_spec live = {{"gpus", "minGpus"}, {8, 8}};
    CHECK(report_attributes(core, latest_question(), &live, 0) == WEIVEL_STATUS_OK);
    advance_to(core, 1000);
    CHECK(report_attributes(core, latest_question(), &live, 1000) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_LOG) == 1);
    CHECK(questions_of(WEIVEL_CAPACITY_SERVE) == 0);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->code == WEIVEL_REFUSAL_UNMET_CONSTRAINT);
    weivel_core_destroy(core);
}

/**
 * A constrained want with no poll interval is malformed: the core mints no interval, and options of an earlier shape
 * give none.
 */
static void a_constrained_want_without_a_poll_interval_is_malformed(void) {
    weivel_core *core = core_with_vocabulary();
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle consumer = open_consumer(core, &constraints, false, NULL);
    forget(core);
    weivel_want_options options = WEIVEL_INIT(weivel_want_options);
    options.budgeted = WEIVEL_CHOICE_YES;
    options.budget_millis = 2000;
    weivel_handle want = 7;
    CHECK(weivel_want_service(core, consumer, SERVICE, &options, &want, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(want == 0);
    CHECK(weivel_want_service(core, consumer, SERVICE, NULL, &want, NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_want_options earlier = WEIVEL_INIT(weivel_want_options);
    earlier.size = (uint32_t)(offsetof(weivel_want_options, timeout_millis) + sizeof(int64_t));
    earlier.poll_millis = 1000;
    CHECK(weivel_want_service(core, consumer, SERVICE, &earlier, &want, NULL) == WEIVEL_STATUS_MALFORMED);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REFUSAL) == 3);
    const seen *refused = nth_of(WEIVEL_EVENT_REFUSAL, 0);
    CHECK(refused->code == WEIVEL_REFUSAL_MALFORMED_CALL);
    check_text(refused->detail,
               "weivel_want_service asked for service \"storage.object\", which the manifest constrains, without a "
               "poll interval",
               "unpolled detail");
    check_text(refused->fix,
               "give weivel_want_options a poll_millis of 1 or more; a budget is optional, and without one the want "
               "polls until a provider meets its constraints",
               "unpolled fix");
    CHECK(count_kind(WEIVEL_EVENT_WANT_SETTLED) == 0);
    int64_t left;
    CHECK(weivel_next_deadline(core, &left, NULL) == WEIVEL_STATUS_NONE);
    weivel_census census;
    weivel_core_census(core, &census);
    CHECK(census.slots == 0);
    weivel_core_destroy(core);
}

static weivel_handle want_unbudgeted(weivel_core *core, weivel_handle plugin, int64_t poll) {
    weivel_want_options options = WEIVEL_INIT(weivel_want_options);
    options.poll_millis = poll;
    weivel_handle want = 0;
    CHECK(weivel_want_service(core, plugin, SERVICE, &options, &want, NULL) == WEIVEL_STATUS_OK);
    CHECK(want != 0);
    return want;
}

/**
 * Without a budget a constrained want polls for as long as it takes (invariant 8): never refused for time, never
 * degraded, and served the moment a provider meets its constraints.
 */
static void a_want_without_a_budget_polls_until_it_is_served(void) {
    enum { POLLS = 120 };
    weivel_core *core = core_with_vocabulary();
    numbers_spec advertised = {{"maxReplicas"}, {1}};
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle provider = open_provider(core, "the-provider", &advertised);
    weivel_handle consumer = open_consumer(core, &constraints, true, NULL);
    weivel_handle registration = register_service(core, provider, SERVICE);
    forget(core);
    weivel_handle want = want_unbudgeted(core, consumer, 1000);
    drain(core);
    uint32_t answered = 0;
    for (int64_t poll = 1; poll <= POLLS; poll++) {
        answered += report_attributes(core, latest_question(), &one_replica, (poll - 1) * 1000) == WEIVEL_STATUS_OK;
        advance_to(core, poll * 1000);
    }
    CHECK(answered == POLLS);
    CHECK(settlement_of(want) == NULL);
    CHECK(questions_of(WEIVEL_CAPACITY_SERVE) == 0);
    CHECK(questions_of(WEIVEL_CAPACITY_ATTRIBUTES) == POLLS + 1);
    advance_to(core, INT64_C(1000000000));
    CHECK(settlement_of(want) == NULL);
    int64_t deadline = 0;
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_OK && deadline == INT64_C(1000001000));
    CHECK(report_attributes(core, latest_question(), &three_replicas, INT64_C(1000000000)) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(report_serves(core, latest_question(), true, INT64_C(1000000000)) == WEIVEL_STATUS_OK);
    drain(core);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->implementation == registration && settled->degraded == 0);
    CHECK(count_kind(WEIVEL_EVENT_LOG) == 0);
    weivel_core_destroy(core);
}

/** Without a budget, a want whose service nothing provides is never refused: it waits for a provider to arrive. */
static void a_want_without_a_budget_outwaits_an_absent_provider(void) {
    weivel_core *core = core_with_vocabulary();
    numbers_spec advertised = {{"maxReplicas"}, {3}};
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle consumer = open_consumer(core, &constraints, false, NULL);
    weivel_handle provider = open_provider(core, "the-provider", &advertised);
    forget(core);
    weivel_handle want = want_unbudgeted(core, consumer, 1000);
    advance_to(core, INT64_C(3600000));
    advance_to(core, INT64_C(86400000));
    CHECK(settlement_of(want) == NULL);
    register_service(core, provider, SERVICE);
    advance_to(core, INT64_C(86401000));
    numbers_spec nothing_live = {{NULL}, {0}};
    CHECK(report_attributes(core, latest_question(), &nothing_live, INT64_C(86401000)) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(report_serves(core, latest_question(), true, INT64_C(86401000)) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(settlement_of(want) != NULL && settlement_of(want)->implementation != 0);
    weivel_core_destroy(core);
}

static uint32_t degraded_reports(void) {
    uint32_t count = 0;
    for (uint32_t at = 0; at < the_journal.count; at++) {
        const seen *item = &the_journal.items[at];
        count += item->kind == WEIVEL_EVENT_LOG && strstr(item->detail, "accepted a degraded") != NULL;
    }
    return count;
}

/** Drives a degradable scene to its spent budget, where the provider is asked under its constraints once more. */
static seen degrade_question(scene *made) {
    advance_to(made->core, 1000);
    advance_to(made->core, 2000);
    advance_to(made->core, 3000);
    seen asked = *latest_question();
    CHECK(asked.question == WEIVEL_CAPACITY_SERVE && asked.constraint_count == 1);
    forget(made->core);
    return asked;
}

/** A yes whose holding cannot be recorded changes nothing and leaves the question open: never settled unreported. */
static void a_yes_that_cannot_be_held_leaves_the_question_open(void) {
    scene made = scene_of(true, 1);
    seen asked = degrade_question(&made);
    made.core->registry->fail_next_holding = true;
    CHECK(report_serves(made.core, &asked, true, 3000) == WEIVEL_STATUS_OUT_OF_MEMORY);
    drain(made.core);
    CHECK(settlement_of(made.want) == NULL);
    CHECK(degraded_reports() == 0);
    CHECK(count_kind(WEIVEL_EVENT_LOG) == 1 && nth_of(WEIVEL_EVENT_LOG, 0)->level == WEIVEL_LOG_ERROR);
    CHECK(questions_of(WEIVEL_CAPACITY_SERVE) == 0);
    CHECK(report_serves(made.core, &asked, true, 3000) == WEIVEL_STATUS_OK);
    drain(made.core);
    const seen *settled = settlement_of(made.want);
    CHECK(settled != NULL && settled->degraded == 1 && settled->implementation == made.registration);
    CHECK(degraded_reports() == 1);
    weivel_core_destroy(made.core);
}

/** Two holdings in a row that cannot be recorded still lose no yes: the third answer settles, reported once. */
static void two_failed_holdings_lose_no_yes(void) {
    scene made = scene_of(true, 1);
    seen asked = degrade_question(&made);
    made.core->registry->fail_next_holding = true;
    CHECK(report_serves(made.core, &asked, true, 3000) == WEIVEL_STATUS_OUT_OF_MEMORY);
    made.core->registry->fail_next_holding = true;
    CHECK(report_serves(made.core, &asked, true, 3001) == WEIVEL_STATUS_OUT_OF_MEMORY);
    drain(made.core);
    CHECK(settlement_of(made.want) == NULL && degraded_reports() == 0);
    CHECK(report_serves(made.core, &asked, true, 3002) == WEIVEL_STATUS_OK);
    drain(made.core);
    const seen *settled = settlement_of(made.want);
    CHECK(settled != NULL && settled->degraded == 1);
    CHECK(degraded_reports() == 1);
    weivel_core_destroy(made.core);
}

/** A poll's yes that cannot be held likewise waits to be answered again, and settles undegraded. */
static void a_poll_yes_that_cannot_be_held_leaves_the_question_open(void) {
    scene made = scene_of(false, 1);
    CHECK(report_attributes(made.core, latest_question(), &three_replicas, 0) == WEIVEL_STATUS_OK);
    drain(made.core);
    seen asked = *latest_question();
    made.core->registry->fail_next_holding = true;
    CHECK(report_serves(made.core, &asked, true, 0) == WEIVEL_STATUS_OUT_OF_MEMORY);
    drain(made.core);
    CHECK(settlement_of(made.want) == NULL);
    CHECK(report_serves(made.core, &asked, true, 0) == WEIVEL_STATUS_OK);
    drain(made.core);
    const seen *settled = settlement_of(made.want);
    CHECK(settled != NULL && settled->degraded == 0 && settled->implementation == made.registration);
    weivel_core_destroy(made.core);
}

/** A budget that would never be spent is a malformed call. */
static void a_budget_that_is_never_spent_is_malformed(void) {
    weivel_core *core = core_with_vocabulary();
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle consumer = open_consumer(core, &constraints, false, NULL);
    weivel_want_options options = WEIVEL_INIT(weivel_want_options);
    options.budgeted = WEIVEL_CHOICE_YES;
    options.budget_millis = 1000;
    options.poll_millis = 0;
    weivel_handle want = 0;
    CHECK(weivel_want_service(core, consumer, SERVICE, &options, &want, NULL) == WEIVEL_STATUS_MALFORMED);
    options.poll_millis = -5;
    CHECK(weivel_want_service(core, consumer, SERVICE, &options, &want, NULL) == WEIVEL_STATUS_MALFORMED);
    options.poll_millis = 1000;
    options.budget_millis = -1;
    CHECK(weivel_want_service(core, consumer, SERVICE, &options, &want, NULL) == WEIVEL_STATUS_MALFORMED);
    options.budget_millis = 0;
    options.budgeted = 3;
    CHECK(weivel_want_service(core, consumer, SERVICE, &options, &want, NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_core_destroy(core);
}

/** A zero budget decides at its only poll, and a late advance skips every poll that has wholly passed. */
static void a_late_advance_skips_the_polls_that_passed(void) {
    weivel_core *core = core_with_vocabulary();
    numbers_spec advertised = {{"maxReplicas"}, {1}};
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle provider = open_provider(core, "the-provider", &advertised);
    weivel_handle consumer = open_consumer(core, &constraints, false, NULL);
    register_service(core, provider, SERVICE);
    forget(core);
    weivel_handle want = want_budgeted(core, consumer, 0, 30000, 1000);
    drain(core);
    advance_to(core, 5500);
    CHECK(questions_of(WEIVEL_CAPACITY_ATTRIBUTES) == 2);
    int64_t deadline = 0;
    CHECK(weivel_next_deadline(core, &deadline, NULL) == WEIVEL_STATUS_OK && deadline == 6000);
    advance_to(core, 100000);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->code == WEIVEL_REFUSAL_UNMET_CONSTRAINT);
    CHECK(questions_of(WEIVEL_CAPACITY_ATTRIBUTES) == 2);
    forget(core);
    weivel_handle instant = want_budgeted(core, consumer, 0, 0, 1000);
    drain(core);
    CHECK(report_attributes(core, latest_question(), &advertised, 0) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(settlement_of(instant) != NULL);
    weivel_core_destroy(core);
}

/** A provider unregistered while asked leaves the question unanswerable and the want to the next poll. */
static void a_provider_that_goes_mid_poll_is_absent_from_then_on(void) {
    scene made = scene_of(true, 1);
    seen first = *latest_question();
    CHECK(weivel_unregister(made.core, made.registration, NULL) == WEIVEL_STATUS_OK);
    CHECK(report_attributes(made.core, &first, &three_replicas, 0) == WEIVEL_STATUS_NONE);
    advance_to(made.core, 1000);
    advance_to(made.core, 2000);
    CHECK(questions_of(WEIVEL_CAPACITY_ATTRIBUTES) == 1);
    const seen *settled = settlement_of(made.want);
    CHECK(settled != NULL && settled->code == WEIVEL_REFUSAL_UNMET_CONSTRAINT);
    if (settled != NULL) {
        mentions(settled->detail, "nothing provides it");
    }
    weivel_core_destroy(made.core);
}

/** A constrained want settles only on a poll, never on a registration arriving between polls. */
static void a_registration_between_polls_waits_for_the_next_poll(void) {
    weivel_core *core = core_with_vocabulary();
    numbers_spec advertised = {{"maxReplicas"}, {3}};
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle consumer = open_consumer(core, &constraints, false, NULL);
    weivel_handle provider = open_provider(core, "the-provider", &advertised);
    forget(core);
    weivel_handle want = want_budgeted(core, consumer, 0, 5000, 1000);
    weivel_handle registration = register_service(core, provider, SERVICE);
    drain(core);
    CHECK(settlement_of(want) == NULL);
    CHECK(questions_of(WEIVEL_CAPACITY_ATTRIBUTES) == 0);
    advance_to(core, 1000);
    seen asked = *latest_question();
    CHECK(asked.question == WEIVEL_CAPACITY_ATTRIBUTES && asked.implementation == registration);
    CHECK(asked.plugin == consumer && asked.subject == want && asked.constraint_count == 1);
    check_text(asked.id, "minReplicas", "constraint named");
    numbers_spec nothing_live = {{NULL}, {0}};
    CHECK(report_attributes(core, &asked, &nothing_live, 1000) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(report_serves(core, latest_question(), true, 1000) == WEIVEL_STATUS_OK);
    drain(core);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->implementation == registration && settled->degraded == 0);
    CHECK(revocations_to(consumer, registration) == 0);
    CHECK(weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(revocations_to(consumer, registration) == 1);
    weivel_core_destroy(core);
}

/** Releasing a consumer mid-poll rejects its want and leaves no deadline or record behind. */
static void releasing_a_polling_consumer_stops_its_want(void) {
    scene made = scene_of(true, 1);
    weivel_census before;
    weivel_core_census(made.core, &before);
    CHECK(before.deadlines == 1);
    CHECK(weivel_release_plugin(made.core, made.consumer, NULL) == WEIVEL_STATUS_OK);
    drain(made.core);
    const seen *settled = settlement_of(made.want);
    CHECK(settled != NULL && settled->code == WEIVEL_REFUSAL_STOPPED);
    weivel_census after;
    weivel_core_census(made.core, &after);
    CHECK(after.deadlines == 0);
    CHECK(after.live_handles == before.live_handles - 1);
    int64_t left;
    CHECK(weivel_next_deadline(made.core, &left, NULL) == WEIVEL_STATUS_NONE);
    weivel_core_destroy(made.core);
}

/** Withdrawing a polling want ends it without an event, its deadline gone. */
static void withdrawing_a_polling_want_ends_it_quietly(void) {
    scene made = scene_of(false, 1);
    seen asked = *latest_question();
    CHECK(weivel_unregister(made.core, made.want, NULL) == WEIVEL_STATUS_OK);
    drain(made.core);
    CHECK(settlement_of(made.want) == NULL);
    int64_t left;
    CHECK(weivel_next_deadline(made.core, &left, NULL) == WEIVEL_STATUS_NONE);
    CHECK(report_attributes(made.core, &asked, &three_replicas, 0) == WEIVEL_STATUS_REFUSED);
    drain(made.core);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_STALE_HANDLE);
    weivel_core_destroy(made.core);
}

/** A report that cannot be read is malformed; one for a want with no constraints, or another registration, is none. */
static void reports_that_answer_nothing(void) {
    scene made = scene_of(false, 1);
    seen asked = *latest_question();
    CHECK(weivel_report_capacity(NULL, made.want, made.registration, NULL, 0, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_report_capacity(made.core, made.want, made.registration, NULL, 0, NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_capacity small = WEIVEL_INIT(weivel_capacity);
    small.size = (uint32_t)offsetof(weivel_capacity, serves);
    CHECK(weivel_report_capacity(made.core, made.want, made.registration, &small, 0, NULL) == WEIVEL_STATUS_MALFORMED);
    weivel_number twice[2] = {WEIVEL_INIT(weivel_number), WEIVEL_INIT(weivel_number)};
    twice[0].name = "maxReplicas";
    twice[1].name = "maxReplicas";
    weivel_capacity repeated = WEIVEL_INIT(weivel_capacity);
    repeated.request = asked.request;
    repeated.attributes.items = twice;
    repeated.attributes.count = 2;
    CHECK(weivel_report_capacity(made.core, made.want, made.registration, &repeated, 0, NULL) ==
          WEIVEL_STATUS_MALFORMED);
    weivel_capacity unread = WEIVEL_INIT(weivel_capacity);
    unread.request = asked.request;
    unread.attributes.count = 1;
    CHECK(weivel_report_capacity(made.core, made.want, made.registration, &unread, 0, NULL) ==
          WEIVEL_STATUS_MALFORMED);
    seen elsewhere = asked;
    elsewhere.implementation = made.provider;
    CHECK(report_attributes(made.core, &elsewhere, &three_replicas, 0) == WEIVEL_STATUS_NONE);
    plugin_spec plain_spec = {"plain", NULL, {SERVICE, "plain:never"}, {NULL}, {NULL}};
    weivel_handle plain = open_spec(made.core, &plain_spec);
    weivel_handle waiting = 0;
    CHECK(weivel_want_service(made.core, plain, "plain:never", NULL, &waiting, NULL) == WEIVEL_STATUS_OK);
    forget(made.core);
    seen not_constrained = asked;
    not_constrained.subject = waiting;
    CHECK(report_attributes(made.core, &not_constrained, &three_replicas, 0) == WEIVEL_STATUS_NONE);
    CHECK(report_attributes(made.core, &asked, &three_replicas, 0) == WEIVEL_STATUS_OK);
    weivel_core_destroy(made.core);
}

/** A plain want on the same id still settles on arrival while a constrained one polls beside it. */
static void a_plain_want_beside_a_constrained_one_settles_on_arrival(void) {
    weivel_core *core = core_with_vocabulary();
    numbers_spec advertised = {{"maxReplicas"}, {3}};
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle consumer = open_consumer(core, &constraints, false, NULL);
    plugin_spec plain_spec = {"plain", NULL, {SERVICE}, {NULL}, {NULL}};
    weivel_handle plain = open_spec(core, &plain_spec);
    weivel_handle provider = open_provider(core, "the-provider", &advertised);
    forget(core);
    weivel_handle constrained = want_budgeted(core, consumer, 0, 5000, 1000);
    weivel_handle unconstrained = want_untimed(core, plain, SERVICE);
    weivel_handle registration = register_service(core, provider, SERVICE);
    drain(core);
    const seen *settled = settlement_of(unconstrained);
    CHECK(settled != NULL && settled->implementation == registration);
    CHECK(settlement_of(constrained) == NULL);
    weivel_core_destroy(core);
}

static weivel_status create_with(const weivel_constraint_entry *entries, uint32_t count, weivel_core **core) {
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.constraint_vocabulary.items = entries;
    options.constraint_vocabulary.count = count;
    return weivel_core_create(&options, core, NULL);
}

/** A vocabulary that cannot be read refuses the core; one given is copied, so the caller's may go at once. */
static void a_vocabulary_is_checked_and_copied(void) {
    weivel_core *core = NULL;
    weivel_constraint_entry unbounded[1] = {entry_of("minReplicas", "maxReplicas", 0)};
    CHECK(create_with(unbounded, 1, &core) == WEIVEL_STATUS_MALFORMED && core == NULL);
    weivel_core_destroy(core);
    weivel_constraint_entry twice[2] = {entry_of("minReplicas", "maxReplicas", WEIVEL_BOUND_LOWER),
                                        entry_of("minReplicas", "replicas", WEIVEL_BOUND_LOWER)};
    CHECK(create_with(twice, 2, &core) == WEIVEL_STATUS_MALFORMED && core == NULL);
    weivel_core_destroy(core);
    weivel_constraint_entry nameless[1] = {entry_of(NULL, "maxReplicas", WEIVEL_BOUND_LOWER)};
    CHECK(create_with(nameless, 1, &core) == WEIVEL_STATUS_MALFORMED && core == NULL);
    weivel_constraint_entry empty_attribute[1] = {entry_of("minReplicas", "", WEIVEL_BOUND_LOWER)};
    CHECK(create_with(empty_attribute, 1, &core) == WEIVEL_STATUS_MALFORMED);
    CHECK(create_with(NULL, 1, &core) == WEIVEL_STATUS_MALFORMED);
    char constraint_name[] = "minReplicas";
    char attribute_name[] = "maxReplicas";
    weivel_constraint_entry copied[1] = {entry_of(constraint_name, attribute_name, WEIVEL_BOUND_LOWER)};
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.well_known_services.items = well_known;
    options.well_known_services.count = 1;
    options.constraint_vocabulary.items = copied;
    options.constraint_vocabulary.count = 1;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    memset(constraint_name, 'x', sizeof constraint_name - 1);
    memset(attribute_name, 'x', sizeof attribute_name - 1);
    copied[0].bound = WEIVEL_BOUND_UPPER;
    numbers_spec advertised = {{"maxReplicas"}, {5}};
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle provider = open_provider(core, "the-provider", &advertised);
    weivel_handle consumer = open_consumer(core, &constraints, false, NULL);
    register_service(core, provider, SERVICE);
    the_journal.count = 0;
    forget(core);
    weivel_handle want = want_budgeted(core, consumer, 0, 0, 1000);
    drain(core);
    numbers_spec nothing_live = {{NULL}, {0}};
    CHECK(report_attributes(core, latest_question(), &nothing_live, 0) == WEIVEL_STATUS_OK);
    drain(core);
    CHECK(report_serves(core, latest_question(), true, 0) == WEIVEL_STATUS_OK);
    drain(core);
    const seen *settled = settlement_of(want);
    CHECK(settled != NULL && settled->implementation != 0 && settled->degraded == 0);
    weivel_core_destroy(core);
}

/** Core options of the shape before the vocabulary know no constraint, so every constraint is unknown. */
static void options_without_a_vocabulary_know_no_constraint(void) {
    weivel_constraint_entry planted[1] = {entry_of("minReplicas", "maxReplicas", WEIVEL_BOUND_LOWER)};
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.well_known_services.items = well_known;
    options.well_known_services.count = 1;
    options.constraint_vocabulary.items = planted;
    options.constraint_vocabulary.count = 1;
    options.size = (uint32_t)offsetof(weivel_core_options, constraint_vocabulary);
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    the_journal.count = 0;
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle consumer = open_consumer(core, &constraints, false, NULL);
    forget(core);
    want_budgeted(core, consumer, 0, 0, 1000);
    drain(core);
    mentions(nth_of(WEIVEL_EVENT_LOG, 0)->detail, "constraint \"minReplicas\" is in no vocabulary");
    weivel_core_destroy(core);
}

/** Many constrained wants poll through one heap: every poll is a deadline, and churn leaves none behind. */
static void many_constrained_wants_leave_nothing_behind(void) {
    enum { WANTS = 500 };
    weivel_core *core = core_with_vocabulary();
    numbers_spec advertised = {{"maxReplicas"}, {1}};
    numbers_spec constraints = {{"minReplicas"}, {3}};
    weivel_handle provider = open_provider(core, "the-provider", &advertised);
    weivel_handle consumer = open_consumer(core, &constraints, true, NULL);
    register_service(core, provider, SERVICE);
    forget(core);
    weivel_census before;
    weivel_core_census(core, &before);
    for (uint32_t index = 0; index < WANTS; index++) {
        want_budgeted(core, consumer, (int64_t)index, 3000, 1000);
    }
    forget(core);
    weivel_census polling;
    weivel_core_census(core, &polling);
    CHECK(polling.deadlines == WANTS);
    uint32_t settled = 0;
    int64_t now = 0;
    for (uint32_t tick = 0; tick < 100000 && weivel_next_deadline(core, &now, NULL) == WEIVEL_STATUS_OK; tick++) {
        CHECK(weivel_advance(core, now, NULL) == WEIVEL_STATUS_OK);
        drain(core);
        settled += count_kind(WEIVEL_EVENT_WANT_SETTLED);
        the_journal.count = 0;
    }
    CHECK(settled == WANTS);
    weivel_census after;
    weivel_core_census(core, &after);
    CHECK(after.deadlines == before.deadlines && after.live_handles == before.live_handles);
    CHECK(after.slots == before.slots && after.holdings == before.holdings);
    weivel_core_destroy(core);
}

/** Replacing a subject's deadline moves the one it has, earlier or later, and never adds a second. */
static void replacing_a_deadline_moves_it(void) {
    weivel_deadlines *deadlines = weivel_deadlines_create();
    CHECK(weivel_deadlines_add(deadlines, 300, 1) == WEIVEL_STATUS_OK);
    CHECK(weivel_deadlines_add(deadlines, 200, 2) == WEIVEL_STATUS_OK);
    CHECK(weivel_deadlines_add(deadlines, 100, 1) == WEIVEL_STATUS_OK);
    CHECK(weivel_deadlines_count(deadlines) == 2);
    int64_t at = 0;
    CHECK(weivel_deadlines_earliest(deadlines, &at) && at == 100);
    CHECK(weivel_deadlines_add(deadlines, 400, 1) == WEIVEL_STATUS_OK);
    CHECK(weivel_deadlines_count(deadlines) == 2);
    weivel_handle subject = 0;
    CHECK(weivel_deadlines_next_due(deadlines, 250, &subject) && subject == 2);
    CHECK(weivel_deadlines_count(deadlines) == 2);
    CHECK(weivel_deadlines_take_due(deadlines, 250, &subject) && subject == 2);
    CHECK(!weivel_deadlines_take_due(deadlines, 250, &subject));
    CHECK(weivel_deadlines_take_due(deadlines, 400, &subject) && subject == 1);
    CHECK(weivel_deadlines_count(deadlines) == 0);
    weivel_deadlines_destroy(deadlines);
}

int main(void) {
    replacing_a_deadline_moves_it();
    a_vocabulary_is_checked_and_copied();
    a_live_attribute_leaves_the_other_advertised_claims_standing();
    an_empty_live_attribute_map_keeps_the_advertised_claim();
    asks_with_the_consumers_constraints_before_falling_back();
    degrades_when_never_satisfied_and_degradable();
    refuses_when_never_satisfied_and_not_degradable();
    serves_when_attributes_satisfy_before_the_deadline();
    every_poll_asks_for_the_attributes_afresh();
    a_poll_left_unanswered_is_not_right_now();
    an_answer_from_an_earlier_poll_counts_for_nothing();
    an_answer_after_its_poll_ended_counts_for_nothing();
    a_degradable_want_waits_out_its_budget();
    a_degradable_want_never_accepts_an_absent_provider();
    a_degradable_want_never_accepts_a_provider_it_is_not_bound_to();
    a_provider_that_serves_nothing_is_refused();
    the_last_poll_refused_still_asks_once_more_before_falling_back();
    an_upper_bound_is_met_only_at_or_below_its_value();
    a_constraint_no_vocabulary_names_is_never_met();
    a_constrained_want_without_a_poll_interval_is_malformed();
    a_want_without_a_budget_polls_until_it_is_served();
    a_want_without_a_budget_outwaits_an_absent_provider();
    a_yes_that_cannot_be_held_leaves_the_question_open();
    two_failed_holdings_lose_no_yes();
    a_poll_yes_that_cannot_be_held_leaves_the_question_open();
    a_budget_that_is_never_spent_is_malformed();
    a_late_advance_skips_the_polls_that_passed();
    a_provider_that_goes_mid_poll_is_absent_from_then_on();
    a_registration_between_polls_waits_for_the_next_poll();
    releasing_a_polling_consumer_stops_its_want();
    withdrawing_a_polling_want_ends_it_quietly();
    reports_that_answer_nothing();
    a_plain_want_beside_a_constrained_one_settles_on_arrival();
    options_without_a_vocabulary_know_no_constraint();
    many_constrained_wants_leave_nothing_behind();
    return check_summary("constraints_test");
}
