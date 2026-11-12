#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <weivel/core.h>

#include "check.h"
#include "core_internal.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#define THREADS 4
#define ROUNDS 1000
#define WANTS_PER_ROUND 3
#define TOTAL_WANTS (THREADS * ROUNDS * WANTS_PER_ROUND)

/** One settlement a thread drained, whichever thread's want it was. */
typedef struct settlement {
    weivel_handle want;
    uint32_t code;
} settlement;

/** What one thread did and what it drained. Everything it tallies, it read off events. */
typedef struct worker {
    weivel_core *core;
    int index;
    weivel_handle wants[ROUNDS * WANTS_PER_ROUND];
    uint32_t want_count;
    weivel_handle kept_consumer;
    weivel_handle kept_want;
    settlement *settled;
    uint32_t settled_count;
    uint32_t revoked;
    uint32_t reach_refused;
    uint32_t refusals;
    uint32_t logs;
    uint32_t capability_provided;
    uint32_t capability_withdrawn;
    uint32_t watched;
    uint32_t failures;
} worker;

static void expect(worker *self, int condition) {
    if (!condition) {
        self->failures++;
    }
}

static void tally(worker *self, const weivel_event *event) {
    switch (event->kind) {
    case WEIVEL_EVENT_WANT_SETTLED:
        if (self->settled_count < TOTAL_WANTS) {
            self->settled[self->settled_count].want = event->data.want.want;
            self->settled[self->settled_count].code = event->data.want.refusal.code;
        }
        self->settled_count++;
        break;
    case WEIVEL_EVENT_REVOKED:
        self->revoked++;
        break;
    case WEIVEL_EVENT_REACH_REFUSED:
        self->reach_refused++;
        break;
    case WEIVEL_EVENT_REFUSAL:
        self->refusals++;
        break;
    case WEIVEL_EVENT_LOG:
        self->logs++;
        break;
    case WEIVEL_EVENT_CAPABILITY:
        self->capability_provided += event->data.capability.change == WEIVEL_CAPABILITY_PROVIDED;
        self->capability_withdrawn += event->data.capability.change == WEIVEL_CAPABILITY_WITHDRAWN;
        break;
    case WEIVEL_EVENT_WATCH:
        self->watched++;
        break;
    default:
        break;
    }
}

static void drain(worker *self) {
    weivel_event event = WEIVEL_INIT(weivel_event);
    while (weivel_next_event(self->core, &event, NULL) == WEIVEL_STATUS_OK) {
        tally(self, &event);
        weivel_event_release(&event);
    }
}

typedef struct names {
    char provider[32];
    char consumer[32];
    char service[48];
    char never[48];
    char capability[32];
} names;

static void name_round(names *names, int thread, int round) {
    snprintf(names->provider, sizeof names->provider, "p%dr%d", thread, round);
    snprintf(names->consumer, sizeof names->consumer, "c%dr%d", thread, round);
    snprintf(names->service, sizeof names->service, "p%dr%d:svc", thread, round);
    snprintf(names->never, sizeof names->never, "p%dr%d:never", thread, round);
    snprintf(names->capability, sizeof names->capability, "cap.%d", thread);
}

static weivel_handle register_provider(worker *self, const names *names) {
    weivel_service_offer offer = WEIVEL_INIT(weivel_service_offer);
    offer.id = names->service;
    const char *capabilities[] = {names->capability};
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = names->provider;
    manifest.api = 1;
    manifest.services.provides.items = &offer;
    manifest.services.provides.count = 1;
    manifest.capabilities.provides.items = capabilities;
    manifest.capabilities.provides.count = 1;
    weivel_handle plugin = 0;
    expect(self, weivel_register_manifest(self->core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    expect(self, weivel_open_plugin(self->core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static weivel_handle register_consumer(worker *self, const names *names) {
    weivel_service_want wants[2] = {WEIVEL_INIT(weivel_service_want), WEIVEL_INIT(weivel_service_want)};
    wants[0].id = names->service;
    wants[1].id = names->never;
    const char *capabilities[] = {names->capability};
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = names->consumer;
    manifest.api = 1;
    manifest.services.consumes.items = wants;
    manifest.services.consumes.count = 2;
    manifest.capabilities.consumes.items = capabilities;
    manifest.capabilities.consumes.count = 1;
    weivel_handle plugin = 0;
    expect(self, weivel_register_manifest(self->core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    expect(self, weivel_open_plugin(self->core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static weivel_handle want(worker *self, weivel_handle plugin, const char *id) {
    weivel_handle handle = 0;
    expect(self, weivel_want_service(self->core, plugin, id, NULL, &handle, NULL) == WEIVEL_STATUS_OK);
    expect(self, handle != 0);
    self->wants[self->want_count++] = handle;
    return handle;
}

/** Reads the ledger while every other thread writes: this round's consumer is in it, active, under its name. */
static void read_ledger(worker *self, weivel_handle consumer, const char *import_name) {
    weivel_ledger_snapshot snapshot = WEIVEL_INIT(weivel_ledger_snapshot);
    expect(self, weivel_ledger(self->core, &snapshot, NULL) == WEIVEL_STATUS_OK);
    int found = 0;
    for (uint32_t index = 0; index < snapshot.row_count; index++) {
        const weivel_ledger_row *row = &snapshot.rows[index];
        if (row->plugin == consumer) {
            found++;
            expect(self, row->status == WEIVEL_PLUGIN_STATUS_ACTIVE && strcmp(row->import_name, import_name) == 0);
        }
    }
    expect(self, found == 1);
    weivel_ledger_snapshot_release(&snapshot);
    weivel_ledger_row row = WEIVEL_INIT(weivel_ledger_row);
    expect(self, weivel_ledger_entry(self->core, consumer, &row, NULL) == WEIVEL_STATUS_OK);
    expect(self, row.status == WEIVEL_PLUGIN_STATUS_ACTIVE && strcmp(row.import_name, import_name) == 0);
    weivel_ledger_row_release(&row);
}

/** Quarantines the consumer for a reason the test gives, then reads that reason back from its row. */
static void quarantine(worker *self, weivel_handle consumer) {
    weivel_quarantine_reason reason = WEIVEL_INIT(weivel_quarantine_reason);
    reason.detail = "the round is over";
    reason.fix = "start the next round";
    expect(self, weivel_quarantine_plugin(self->core, consumer, &reason, NULL) == WEIVEL_STATUS_OK);
    weivel_ledger_row row = WEIVEL_INIT(weivel_ledger_row);
    expect(self, weivel_ledger_entry(self->core, consumer, &row, NULL) == WEIVEL_STATUS_OK);
    expect(self, row.status == WEIVEL_PLUGIN_STATUS_QUARANTINED && row.refusal.detail != NULL &&
                     strcmp(row.refusal.detail, "the round is over") == 0);
    weivel_ledger_row_release(&row);
}

/**
 * One round: a provider and a consumer meet and part, every step followed by a drain. The consumer watches the
 * service and stops watching, watches the capability for good, and is quarantined for a reason before it goes. The
 * consumer of the last round stays open, its want for a service nothing provides still waiting and its capability
 * watch still installed, so the end state holds both.
 */
static void run_round(worker *self, int round) {
    names names;
    name_round(&names, self->index, round);
    weivel_core *core = self->core;
    weivel_handle provider = register_provider(self, &names);
    weivel_handle consumer = register_consumer(self, &names);
    want(self, consumer, names.service);
    weivel_handle never = want(self, consumer, names.never);
    weivel_handle service_watch = 0;
    expect(self, weivel_watch_service(core, consumer, names.service, &service_watch, NULL) == WEIVEL_STATUS_OK);
    expect(self, weivel_unregister(core, service_watch, NULL) == WEIVEL_STATUS_OK);
    weivel_handle capability_watch = 0;
    expect(self, weivel_watch_capability(core, consumer, names.capability, &capability_watch, NULL) ==
                     WEIVEL_STATUS_OK);
    read_ledger(self, consumer, names.consumer);
    drain(self);
    weivel_handle registration = 0;
    expect(self, weivel_register_service(core, provider, names.service, &registration, NULL) == WEIVEL_STATUS_OK);
    want(self, consumer, names.service);
    weivel_handle got = 0;
    expect(self, weivel_get_service(core, consumer, "nowhere:x", &got, NULL) == WEIVEL_STATUS_REFUSED);
    drain(self);
    weivel_handle implementation = 0;
    expect(self, weivel_provide_capability(core, provider, names.capability, &implementation, NULL) ==
                     WEIVEL_STATUS_OK);
    expect(self, weivel_verify_activation(core, provider, NULL) == WEIVEL_STATUS_OK);
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    expect(self, weivel_all_capabilities(core, consumer, names.capability, &list, NULL) == WEIVEL_STATUS_OK);
    expect(self, list.count == 1 && list.items != NULL && list.items[0].implementation == implementation);
    weivel_capability_list_release(&list);
    drain(self);
    expect(self, weivel_unregister(core, registration, NULL) == WEIVEL_STATUS_OK);
    read_ledger(self, consumer, names.consumer);
    expect(self, weivel_release_plugin(core, provider, NULL) == WEIVEL_STATUS_OK);
    expect(self, weivel_forget_plugin(core, provider, NULL) == WEIVEL_STATUS_OK);
    drain(self);
    if (round == ROUNDS - 1) {
        self->kept_consumer = consumer;
        self->kept_want = never;
        return;
    }
    quarantine(self, consumer);
    expect(self, weivel_release_plugin(core, consumer, NULL) == WEIVEL_STATUS_OK);
    expect(self, weivel_forget_plugin(core, consumer, NULL) == WEIVEL_STATUS_OK);
    drain(self);
}

static void work(worker *self) {
    for (int round = 0; round < ROUNDS; round++) {
        run_round(self, round);
    }
}

#if defined(_WIN32)
static DWORD WINAPI run_worker(LPVOID argument) {
    work(argument);
    return 0;
}
#else
static void *run_worker(void *argument) {
    work(argument);
    return NULL;
}
#endif

static void run_all(worker *workers) {
#if defined(_WIN32)
    HANDLE threads[THREADS - 1];
    for (int index = 1; index < THREADS; index++) {
        threads[index - 1] = CreateThread(NULL, 0, run_worker, &workers[index], 0, NULL);
        CHECK(threads[index - 1] != NULL);
    }
    work(&workers[0]);
    WaitForMultipleObjects(THREADS - 1, threads, TRUE, INFINITE);
    for (int index = 0; index < THREADS - 1; index++) {
        CloseHandle(threads[index]);
    }
#else
    pthread_t threads[THREADS - 1];
    for (int index = 1; index < THREADS; index++) {
        CHECK(pthread_create(&threads[index - 1], NULL, run_worker, &workers[index]) == 0);
    }
    work(&workers[0]);
    for (int index = 0; index < THREADS - 1; index++) {
        pthread_join(threads[index], NULL);
    }
#endif
}

/** Every settlement any thread drained. */
typedef struct settlements {
    settlement *items;
    uint32_t count;
} settlements;

static settlements gather(worker *workers, worker *rest) {
    settlements all = {calloc(TOTAL_WANTS * 2, sizeof(settlement)), 0};
    for (int index = 0; index <= THREADS; index++) {
        worker *from = index < THREADS ? &workers[index] : rest;
        CHECK(from->settled_count <= TOTAL_WANTS);
        for (uint32_t at = 0; at < from->settled_count && at < TOTAL_WANTS; at++) {
            all.items[all.count++] = from->settled[at];
        }
    }
    return all;
}

static uint32_t settlements_of(const settlements *all, weivel_handle want, uint32_t *code) {
    uint32_t found = 0;
    for (uint32_t at = 0; at < all->count; at++) {
        if (all->items[at].want == want) {
            *code = all->items[at].code;
            found++;
        }
    }
    return found;
}

static uint32_t sum(const worker *workers, const worker *rest, size_t offset) {
    uint32_t total = *(const uint32_t *)(const void *)((const char *)rest + offset);
    for (int index = 0; index < THREADS; index++) {
        total += *(const uint32_t *)(const void *)((const char *)&workers[index] + offset);
    }
    return total;
}

static int contains(const weivel_strings *strings, const char *text) {
    for (uint32_t index = 0; index < strings->count; index++) {
        if (strcmp(strings->items[index], text) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Each kept consumer's row says what its operations made it: active, the two ids it asked for, one refusal. */
static void check_ledger(weivel_core *core, const worker *workers) {
    weivel_ledger_snapshot snapshot = WEIVEL_INIT(weivel_ledger_snapshot);
    CHECK(weivel_ledger(core, &snapshot, NULL) == WEIVEL_STATUS_OK);
    CHECK(snapshot.row_count == THREADS);
    CHECK(snapshot.refusal_count == THREADS);
    for (uint32_t index = 0; index < snapshot.row_count && index < THREADS; index++) {
        const weivel_ledger_row *row = &snapshot.rows[index];
        int owner = -1;
        for (int thread = 0; thread < THREADS; thread++) {
            owner = workers[thread].kept_consumer == row->plugin ? thread : owner;
        }
        CHECK(owner >= 0);
        if (owner < 0) {
            continue;
        }
        names names;
        name_round(&names, owner, ROUNDS - 1);
        check_text(row->import_name, names.consumer, "kept consumer");
        CHECK(row->status == WEIVEL_PLUGIN_STATUS_ACTIVE);
        CHECK(row->services_consumed.count == 2);
        if (row->services_consumed.count == 2) {
            check_text(row->services_consumed.items[0], names.service, "asked first");
            check_text(row->services_consumed.items[1], names.never, "asked second");
        }
        CHECK(row->services_provided.count == 0 && row->capabilities_provided.count == 0);
        CHECK(!contains(&row->services_consumed, "nowhere:x"));
    }
    for (uint32_t index = 0; index < snapshot.refusal_count; index++) {
        int kept = 0;
        for (int thread = 0; thread < THREADS; thread++) {
            kept |= workers[thread].kept_consumer == snapshot.refusals[index].plugin;
        }
        CHECK(kept);
        check_text(snapshot.refusals[index].id, "nowhere:x", "the kept consumer's refusal");
    }
    weivel_ledger_snapshot_release(&snapshot);
}

/** Checks the census: `kept` consumers each still hold a plugin, a want and a capability watch, and nothing else. */
static void check_census(weivel_core *core, const weivel_census *baseline, uint32_t kept) {
    weivel_census census;
    weivel_core_census(core, &census);
    CHECK(census.plugins == baseline->plugins + kept);
    CHECK(census.live_handles == baseline->live_handles + 3 * kept);
    CHECK(census.slots == baseline->slots + kept);
    CHECK(census.holdings == baseline->holdings);
    CHECK(census.deadlines == baseline->deadlines);
    CHECK(census.capabilities == baseline->capabilities + kept);
    CHECK(census.provisions == baseline->provisions);
    CHECK(census.capability_watches == baseline->capability_watches + kept);
    CHECK(census.capability_holdings == baseline->capability_holdings);
}

static void check_every_want_accounted_for(const worker *workers, const settlements *all, int kept_pending) {
    uint32_t stopped = 0;
    uint32_t served = 0;
    for (int thread = 0; thread < THREADS; thread++) {
        for (uint32_t index = 0; index < workers[thread].want_count; index++) {
            weivel_handle handle = workers[thread].wants[index];
            uint32_t code = 0;
            uint32_t count = settlements_of(all, handle, &code);
            if (kept_pending && handle == workers[thread].kept_want) {
                CHECK(count == 0);
                continue;
            }
            CHECK(count == 1);
            stopped += code == WEIVEL_REFUSAL_STOPPED;
            served += code == WEIVEL_REFUSAL_NONE;
        }
    }
    uint32_t still_waiting = kept_pending ? THREADS : 0;
    CHECK(served == THREADS * ROUNDS * 2);
    CHECK(stopped + still_waiting == THREADS * ROUNDS);
    CHECK(all->count == TOTAL_WANTS - still_waiting);
}

static void threads_meeting_and_parting_on_one_core_leave_it_consistent(void) {
    weivel_core *core = NULL;
    CHECK(weivel_core_create(NULL, &core, NULL) == WEIVEL_STATUS_OK);
    weivel_census baseline;
    weivel_core_census(core, &baseline);
    worker *workers = calloc(THREADS + 1, sizeof *workers);
    for (int index = 0; index <= THREADS; index++) {
        workers[index].core = core;
        workers[index].index = index;
        workers[index].settled = calloc(TOTAL_WANTS, sizeof(settlement));
    }
    worker *rest = &workers[THREADS];

    run_all(workers);
    drain(rest);
    for (int index = 0; index < THREADS; index++) {
        CHECK(workers[index].failures == 0);
        CHECK(workers[index].want_count == ROUNDS * WANTS_PER_ROUND);
    }
    CHECK(sum(workers, rest, offsetof(worker, revoked)) == THREADS * ROUNDS * 2);
    CHECK(sum(workers, rest, offsetof(worker, reach_refused)) == THREADS * ROUNDS);
    CHECK(sum(workers, rest, offsetof(worker, refusals)) == 0);
    CHECK(sum(workers, rest, offsetof(worker, logs)) == 0);
    CHECK(sum(workers, rest, offsetof(worker, capability_provided)) == THREADS * ROUNDS);
    CHECK(sum(workers, rest, offsetof(worker, capability_withdrawn)) == THREADS * ROUNDS);
    CHECK(sum(workers, rest, offsetof(worker, watched)) == 0);
    settlements pending = gather(workers, rest);
    check_every_want_accounted_for(workers, &pending, 1);
    free(pending.items);
    check_ledger(core, workers);
    check_census(core, &baseline, THREADS);

    for (int thread = 0; thread < THREADS; thread++) {
        CHECK(weivel_release_plugin(core, workers[thread].kept_consumer, NULL) == WEIVEL_STATUS_OK);
        CHECK(weivel_forget_plugin(core, workers[thread].kept_consumer, NULL) == WEIVEL_STATUS_OK);
    }
    drain(rest);
    settlements all = gather(workers, rest);
    check_every_want_accounted_for(workers, &all, 0);
    free(all.items);
    check_census(core, &baseline, 0);

    weivel_ledger_snapshot snapshot = WEIVEL_INIT(weivel_ledger_snapshot);
    CHECK(weivel_ledger(core, &snapshot, NULL) == WEIVEL_STATUS_OK);
    CHECK(snapshot.row_count == 0 && snapshot.refusal_count == 0);
    weivel_ledger_snapshot_release(&snapshot);
    for (int index = 0; index <= THREADS; index++) {
        free(workers[index].settled);
    }
    free(workers);
    weivel_core_destroy(core);
}

int main(void) {
    threads_meeting_and_parting_on_one_core_leave_it_consistent();
    return check_summary("threads_test");
}
