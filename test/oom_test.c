#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <weivel/core.h>

#include "check.h"
#include "core_internal.h"
#include "memory.h"

/**
 * Proves every mutating call all or nothing under memory exhaustion: for each call, the seam fails its first
 * allocation, then its second, and so on until the call no longer reaches the failing one. At every step the core
 * must either answer WEIVEL_STATUS_OUT_OF_MEMORY and be exactly as it was before the call (census, ledger and the
 * events it queued), or answer what an unfailed run answers and be exactly as that run left it. Between calls the
 * core must hold one reserved event per want, holding and capability holding, and no other.
 */

#define TEXT_CAPACITY 16384

typedef struct picture {
    weivel_census census;
    char text[TEXT_CAPACITY];
    size_t length;
} picture;

static void put(picture *into, const char *format, unsigned long long first, unsigned long long second,
                unsigned long long third) {
    if (into->length >= TEXT_CAPACITY - 1) {
        return;
    }
    int wrote = snprintf(into->text + into->length, TEXT_CAPACITY - into->length, format, first, second, third);
    if (wrote > 0) {
        into->length += (size_t)wrote;
    }
}

static void put_text(picture *into, const char *text) {
    if (into->length >= TEXT_CAPACITY - 1) {
        return;
    }
    int wrote = snprintf(into->text + into->length, TEXT_CAPACITY - into->length, "%s;", text == NULL ? "" : text);
    if (wrote > 0) {
        into->length += (size_t)wrote;
    }
}

static void put_strings(picture *into, const weivel_strings *strings) {
    put(into, "[%llu]", strings->count, 0, 0);
    for (uint32_t index = 0; index < strings->count; index++) {
        put_text(into, strings->items[index]);
    }
}

static void put_ledger(weivel_core *core, picture *into) {
    weivel_ledger_snapshot snapshot = WEIVEL_INIT(weivel_ledger_snapshot);
    CHECK(weivel_ledger(core, &snapshot, NULL) == WEIVEL_STATUS_OK);
    for (uint32_t index = 0; index < snapshot.row_count; index++) {
        const weivel_ledger_row *row = &snapshot.rows[index];
        put(into, "row %llu %llu %llu ", row->plugin, row->status, row->verified);
        put(into, "refusal %llu ", row->refusal.code, 0, 0);
        put_text(into, row->import_name);
        put_strings(into, &row->services_provided);
        put_strings(into, &row->capabilities_provided);
        put_strings(into, &row->services_consumed);
        put_strings(into, &row->capabilities_consumed);
    }
    for (uint32_t index = 0; index < snapshot.refusal_count; index++) {
        const weivel_declaration_refusal *refusal = &snapshot.refusals[index];
        put(into, "reported %llu %llu %llu ", refusal->kind, refusal->plugin, refusal->code);
        put_text(into, refusal->id);
    }
    weivel_ledger_snapshot_release(&snapshot);
}

/** Drains the queue into the picture, every event but a log, which the core may drop, by its kind and handles. */
static void put_events(weivel_core *core, picture *into) {
    weivel_event event = WEIVEL_INIT(weivel_event);
    while (weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK) {
        switch (event.kind) {
        case WEIVEL_EVENT_REFUSAL:
            put(into, "refusal %llu|", event.data.refusal.code, 0, 0);
            break;
        case WEIVEL_EVENT_WANT_SETTLED:
            put(into, "settled %llu %llu %llu ", event.data.want.want, event.data.want.plugin,
                event.data.want.implementation);
            put(into, "%llu %llu|", event.data.want.refusal.code, event.data.want.degraded, 0);
            break;
        case WEIVEL_EVENT_WATCH:
            put(into, "watch %llu %llu %llu ", event.data.watch.watch, event.data.watch.plugin,
                event.data.watch.implementation);
            put(into, "%llu|", event.data.watch.change, 0, 0);
            break;
        case WEIVEL_EVENT_REVOKED:
            put(into, "revoked %llu %llu|", event.data.revoked.plugin, event.data.revoked.implementation, 0);
            break;
        case WEIVEL_EVENT_REACH_REFUSED:
            put(into, "reach %llu %llu %llu ", event.data.reach.kind, event.data.reach.plugin,
                event.data.reach.refusal.code);
            put_text(into, event.data.reach.id);
            break;
        case WEIVEL_EVENT_CAPACITY_WANTED:
            put(into, "asked %llu %llu %llu ", event.data.capacity.want, event.data.capacity.registration,
                event.data.capacity.question);
            put(into, "%llu|", event.data.capacity.request, 0, 0);
            break;
        case WEIVEL_EVENT_CAPABILITY:
            put(into, "capability %llu %llu %llu ", event.data.capability.watch, event.data.capability.plugin,
                event.data.capability.implementation);
            put(into, "%llu|", event.data.capability.change, 0, 0);
            break;
        default:
            break;
        }
        weivel_event_release(&event);
    }
}

static void take_picture(weivel_core *core, picture *into) {
    memset(into, 0, sizeof *into);
    weivel_core_census(core, &into->census);
    put_events(core, into);
    put(into, "#", 0, 0, 0);
    put_ledger(core, into);
}

static bool same_picture(const picture *left, const picture *right) {
    return memcmp(&left->census, &right->census, sizeof left->census) == 0 && strcmp(left->text, right->text) == 0;
}

/** Between calls every reservation outstanding is one an obligation holds. */
static bool reservations_balance(const weivel_census *census) {
    return census->reserved_events == census->wants + census->holdings + census->capability_holdings;
}

/** The plugins and handles a scene starts from. */
typedef struct world {
    weivel_core *core;
    weivel_handle calendar;
    weivel_handle notes;
    weivel_handle agenda;
    weivel_handle board;
    weivel_handle picky;
    weivel_handle registration;
    weivel_handle provision;
    weivel_handle watch;
    weivel_handle want;
} world;

typedef void (*scene_setup)(world *);
typedef weivel_status (*scene_act)(world *);

typedef struct spec {
    const char *name;
    const char *consumes[3];
    const char *providers[3];
    const char *provides[2];
    const char *capabilities_provided[2];
    const char *capabilities_consumed[2];
    const weivel_number *constraint;
} spec;

static uint32_t count_of(const char *const *items, uint32_t capacity) {
    uint32_t count = 0;
    while (count < capacity && items[count] != NULL) {
        count++;
    }
    return count;
}

static weivel_handle open_plugin(weivel_core *core, const spec *plugin) {
    weivel_service_want wants[3];
    weivel_service_offer offers[2];
    uint32_t consumed = count_of(plugin->consumes, 3);
    uint32_t provided = count_of(plugin->provides, 2);
    for (uint32_t index = 0; index < consumed; index++) {
        weivel_service_want want = WEIVEL_INIT(weivel_service_want);
        want.id = plugin->consumes[index];
        want.provider = plugin->providers[index];
        if (plugin->constraint != NULL) {
            want.constraints.items = plugin->constraint;
            want.constraints.count = 1;
        }
        wants[index] = want;
    }
    for (uint32_t index = 0; index < provided; index++) {
        weivel_service_offer offer = WEIVEL_INIT(weivel_service_offer);
        offer.id = plugin->provides[index];
        offers[index] = offer;
    }
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = plugin->name;
    manifest.api = 1;
    manifest.services.consumes.items = consumed == 0 ? NULL : wants;
    manifest.services.consumes.count = consumed;
    manifest.services.provides.items = provided == 0 ? NULL : offers;
    manifest.services.provides.count = provided;
    manifest.capabilities.provides.items = plugin->capabilities_provided;
    manifest.capabilities.provides.count = count_of(plugin->capabilities_provided, 2);
    manifest.capabilities.consumes.items = plugin->capabilities_consumed;
    manifest.capabilities.consumes.count = count_of(plugin->capabilities_consumed, 2);
    weivel_handle handle = 0;
    CHECK(weivel_register_manifest(core, &manifest, &handle, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, handle, NULL, NULL) == WEIVEL_STATUS_OK);
    return handle;
}

static const weivel_number replicas = {(uint32_t)sizeof(weivel_number), "replicas", 3};

static void make_world(world *made) {
    memset(made, 0, sizeof *made);
    weivel_constraint_entry entry = WEIVEL_INIT(weivel_constraint_entry);
    entry.constraint_name = "replicas";
    entry.attribute_name = "replicas";
    entry.bound = WEIVEL_BOUND_LOWER;
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.constraint_vocabulary.items = &entry;
    options.constraint_vocabulary.count = 1;
    options.initial_event_capacity = 1;
    CHECK(weivel_core_create(&options, &made->core, NULL) == WEIVEL_STATUS_OK);
    spec calendar = {"calendar", {"clock"}, {NULL}, {"calendar:events"}, {"screens"}, {NULL}, NULL};
    spec notes = {"notes", {"calendar:events", "clock"}, {NULL}, {NULL}, {NULL}, {"screens"}, NULL};
    spec agenda = {"agenda", {"calendar:events"}, {"other"}, {NULL}, {NULL}, {NULL}, NULL};
    spec board = {"board", {"calendar:events", "clock"}, {NULL}, {NULL}, {NULL}, {"screens"}, NULL};
    spec picky = {"picky", {"calendar:events"}, {NULL}, {NULL}, {NULL}, {NULL}, &replicas};
    made->calendar = open_plugin(made->core, &calendar);
    made->notes = open_plugin(made->core, &notes);
    made->agenda = open_plugin(made->core, &agenda);
    made->board = open_plugin(made->core, &board);
    made->picky = open_plugin(made->core, &picky);
}

static void register_events(world *made) {
    CHECK(weivel_register_service(made->core, made->calendar, "calendar:events", &made->registration, NULL) ==
          WEIVEL_STATUS_OK);
}

static void provide_screens(world *made) {
    CHECK(weivel_provide_capability(made->core, made->calendar, "screens", &made->provision, NULL) ==
          WEIVEL_STATUS_OK);
}

static void watch_events(world *made, weivel_handle plugin) {
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(made->core, plugin, "calendar:events", &watch, NULL) == WEIVEL_STATUS_OK);
    made->watch = watch;
}

static void watch_screens(world *made, weivel_handle plugin) {
    weivel_handle watch = 0;
    CHECK(weivel_watch_capability(made->core, plugin, "screens", &watch, NULL) == WEIVEL_STATUS_OK);
}

static void get_events(world *made, weivel_handle plugin) {
    weivel_handle got = 0;
    CHECK(weivel_get_service(made->core, plugin, "calendar:events", &got, NULL) == WEIVEL_STATUS_OK);
}

static void all_screens(world *made, weivel_handle plugin) {
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    CHECK(weivel_all_capabilities(made->core, plugin, "screens", &list, NULL) == WEIVEL_STATUS_OK);
    weivel_capability_list_release(&list);
}

static void want_events(world *made, weivel_handle plugin) {
    CHECK(weivel_want_service(made->core, plugin, "calendar:events", NULL, &made->want, NULL) == WEIVEL_STATUS_OK);
}

static void drain_away(weivel_core *core) {
    weivel_event event = WEIVEL_INIT(weivel_event);
    while (weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK) {
        weivel_event_release(&event);
    }
}

/** Runs the call once unfailed, then fails each of its allocations in turn, checking all or nothing at each. */
static void sweep(const char *name, scene_setup setup, scene_act act) {
    world made;
    setup(&made);
    drain_away(made.core);
    weivel_status expected = act(&made);
    static picture reference;
    take_picture(made.core, &reference);
    CHECK(reservations_balance(&reference.census));
    weivel_core_destroy(made.core);
    uint32_t points = 0;
    int failures_before = check_failures;
    for (uint64_t nth = 1; nth < 100000; nth++) {
        setup(&made);
        drain_away(made.core);
        static picture before;
        take_picture(made.core, &before);
        weivel_memory_fail_at(nth);
        weivel_status status = act(&made);
        bool failed = weivel_memory_failed();
        weivel_memory_fail_at(0);
        static picture after;
        take_picture(made.core, &after);
        weivel_core_destroy(made.core);
        bool balanced = reservations_balance(&after.census);
        bool undone = status == WEIVEL_STATUS_OUT_OF_MEMORY && same_picture(&before, &after);
        bool completed = status == expected && same_picture(&reference, &after);
        if (!balanced || (!undone && !completed)) {
            fprintf(stderr, "%s: allocation %llu failing answered %d and left the core %s\n", name,
                    (unsigned long long)nth, (int)status, balanced ? "neither as before nor complete" : "unbalanced");
            fprintf(stderr, "  before:   %s\n  after:    %s\n  complete: %s\n", before.text, after.text,
                    reference.text);
            check_failures++;
        }
        if (!failed) {
            CHECK(completed);
            break;
        }
        points++;
    }
    if (check_failures == failures_before && points == 0) {
        printf("%s: allocates nothing, so running out of memory cannot touch it\n", name);
    } else if (check_failures == failures_before) {
        printf("%s: %u allocations, each failing all or nothing\n", name, (unsigned)points);
    }
}

static void world_with_registration(world *made) {
    make_world(made);
    register_events(made);
}

static weivel_status notes_get(world *made) {
    weivel_handle got = 0;
    return weivel_get_service(made->core, made->notes, "calendar:events", &got, NULL);
}

static weivel_status notes_want(world *made) {
    weivel_handle want = 0;
    return weivel_want_service(made->core, made->notes, "calendar:events", NULL, &want, NULL);
}

static weivel_status notes_want_timed(world *made) {
    weivel_want_options options = WEIVEL_INIT(weivel_want_options);
    options.timed = WEIVEL_CHOICE_YES;
    options.now = 10;
    options.timeout_millis = 50;
    weivel_handle want = 0;
    return weivel_want_service(made->core, made->notes, "calendar:events", &options, &want, NULL);
}

static weivel_status picky_want(world *made) {
    weivel_want_options options = WEIVEL_INIT(weivel_want_options);
    options.poll_millis = 100;
    weivel_handle want = 0;
    return weivel_want_service(made->core, made->picky, "calendar:events", &options, &want, NULL);
}

static weivel_status agenda_want_bound_elsewhere(world *made) {
    weivel_handle want = 0;
    return weivel_want_service(made->core, made->agenda, "calendar:events", NULL, &want, NULL);
}

static weivel_status board_watch(world *made) {
    weivel_handle watch = 0;
    return weivel_watch_service(made->core, made->board, "calendar:events", &watch, NULL);
}

static weivel_status board_watch_screens(world *made) {
    weivel_handle watch = 0;
    return weivel_watch_capability(made->core, made->board, "screens", &watch, NULL);
}

static void world_awaiting_events(world *made) {
    make_world(made);
    want_events(made, made->notes);
    want_events(made, made->agenda);
    watch_events(made, made->board);
    watch_events(made, made->notes);
}

static weivel_status calendar_registers(world *made) {
    weivel_handle registration = 0;
    return weivel_register_service(made->core, made->calendar, "calendar:events", &registration, NULL);
}

static void world_with_a_host_clock(world *made) {
    make_world(made);
    weivel_handle clock = 0;
    CHECK(weivel_register_service(made->core, WEIVEL_HOST, "clock", &clock, NULL) == WEIVEL_STATUS_OK);
    weivel_handle got = 0;
    CHECK(weivel_get_service(made->core, made->notes, "clock", &got, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_watch_service(made->core, made->board, "clock", &got, NULL) == WEIVEL_STATUS_OK);
}

static weivel_status host_registers_clock_again(world *made) {
    weivel_handle clock = 0;
    return weivel_register_service(made->core, WEIVEL_HOST, "clock", &clock, NULL);
}

static void world_with_holders(world *made) {
    make_world(made);
    register_events(made);
    watch_events(made, made->board);
    get_events(made, made->notes);
    get_events(made, made->board);
}

static weivel_status unregister_events(world *made) {
    return weivel_unregister(made->core, made->registration, NULL);
}

static void world_with_two_verified_screens(world *made) {
    make_world(made);
    provide_screens(made);
    CHECK(weivel_verify_activation(made->core, made->calendar, NULL) == WEIVEL_STATUS_OK);
    spec second = {"projector", {NULL}, {NULL}, {NULL}, {"screens"}, {NULL}, NULL};
    weivel_handle projector = open_plugin(made->core, &second);
    weivel_handle implementation = 0;
    CHECK(weivel_provide_capability(made->core, projector, "screens", &implementation, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_verify_activation(made->core, projector, NULL) == WEIVEL_STATUS_OK);
}

static weivel_status notes_all(world *made) {
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    weivel_status status = weivel_all_capabilities(made->core, made->notes, "screens", &list, NULL);
    weivel_capability_list_release(&list);
    return status;
}

static void world_watching_screens(world *made) {
    make_world(made);
    watch_screens(made, made->board);
    watch_screens(made, made->notes);
    provide_screens(made);
}

static weivel_status calendar_verifies(world *made) {
    return weivel_verify_activation(made->core, made->calendar, NULL);
}

static weivel_status calendar_provides(world *made) {
    weivel_handle implementation = 0;
    return weivel_provide_capability(made->core, made->calendar, "screens", &implementation, NULL);
}

/** A provider with everything a release takes: holders and watches of its service and capability, and wants. */
static void world_at_full_stretch(world *made) {
    make_world(made);
    weivel_handle clock = 0;
    CHECK(weivel_register_service(made->core, WEIVEL_HOST, "clock", &clock, NULL) == WEIVEL_STATUS_OK);
    weivel_handle got = 0;
    CHECK(weivel_get_service(made->core, made->calendar, "clock", &got, NULL) == WEIVEL_STATUS_OK);
    weivel_handle waiting = 0;
    CHECK(weivel_unregister(made->core, clock, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_want_service(made->core, made->calendar, "clock", NULL, &waiting, NULL) == WEIVEL_STATUS_OK);
    watch_screens(made, made->board);
    watch_screens(made, made->calendar);
    provide_screens(made);
    CHECK(weivel_verify_activation(made->core, made->calendar, NULL) == WEIVEL_STATUS_OK);
    all_screens(made, made->notes);
    watch_events(made, made->board);
    register_events(made);
    get_events(made, made->notes);
}

static weivel_status release_calendar(world *made) {
    return weivel_release_plugin(made->core, made->calendar, NULL);
}

static weivel_status quarantine_calendar(world *made) {
    weivel_quarantine_reason reason = WEIVEL_INIT(weivel_quarantine_reason);
    reason.detail = "its activation threw";
    reason.fix = "mend it";
    return weivel_quarantine_plugin(made->core, made->calendar, &reason, NULL);
}

/** A released plugin that still holds what its late calls were handed. */
static void world_with_a_late_holder(world *made) {
    world_at_full_stretch(made);
    CHECK(weivel_release_plugin(made->core, made->notes, NULL) == WEIVEL_STATUS_OK);
    get_events(made, made->notes);
    all_screens(made, made->notes);
}

static weivel_status forget_notes(world *made) {
    return weivel_forget_plugin(made->core, made->notes, NULL);
}

static weivel_status host_wants(world *made) {
    weivel_handle want = 0;
    return weivel_want_service(made->core, WEIVEL_HOST, "calendar:events", NULL, &want, NULL);
}

static weivel_status host_gets(world *made) {
    weivel_handle got = 0;
    return weivel_get_service(made->core, WEIVEL_HOST, "calendar:events", &got, NULL);
}

static weivel_status host_watches(world *made) {
    weivel_handle watch = 0;
    return weivel_watch_service(made->core, WEIVEL_HOST, "calendar:events", &watch, NULL);
}

static weivel_status host_watches_screens(world *made) {
    weivel_handle watch = 0;
    return weivel_watch_capability(made->core, WEIVEL_HOST, "screens", &watch, NULL);
}

static weivel_status host_all(world *made) {
    weivel_capability_list list = WEIVEL_INIT(weivel_capability_list);
    weivel_status status = weivel_all_capabilities(made->core, WEIVEL_HOST, "screens", &list, NULL);
    weivel_capability_list_release(&list);
    return status;
}

static weivel_status register_a_manifest(world *made) {
    spec newcomer = {"newcomer", {"calendar:events"}, {NULL}, {"newcomer:feed"}, {"screens"}, {NULL}, NULL};
    weivel_service_want want = WEIVEL_INIT(weivel_service_want);
    want.id = newcomer.consumes[0];
    weivel_service_offer offer = WEIVEL_INIT(weivel_service_offer);
    offer.id = newcomer.provides[0];
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = newcomer.name;
    manifest.api = 1;
    manifest.services.consumes.items = &want;
    manifest.services.consumes.count = 1;
    manifest.services.provides.items = &offer;
    manifest.services.provides.count = 1;
    manifest.capabilities.provides.items = newcomer.capabilities_provided;
    manifest.capabilities.provides.count = 1;
    weivel_handle plugin = 0;
    return weivel_register_manifest(made->core, &manifest, &plugin, NULL);
}

static weivel_status register_a_malformed_manifest(world *made) {
    const char *twice[2] = {"screens", "screens"};
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "doubled";
    manifest.capabilities.provides.items = twice;
    manifest.capabilities.provides.count = 2;
    weivel_handle plugin = 0;
    return weivel_register_manifest(made->core, &manifest, &plugin, NULL);
}

/** Asking whether a malformed manifest is supported answers its whole refusal, or out of memory, never less. */
static void supports_answers_a_whole_refusal_or_none(void) {
    const char *twice[2] = {"screens", "screens"};
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "doubled";
    manifest.capabilities.provides.items = twice;
    manifest.capabilities.provides.count = 2;
    world made;
    make_world(&made);
    uint32_t points = 0;
    for (uint64_t nth = 1; nth < 1000; nth++) {
        weivel_support answer = WEIVEL_INIT(weivel_support);
        weivel_memory_fail_at(nth);
        weivel_status status = weivel_supports(made.core, &manifest, &answer, NULL);
        bool failed = weivel_memory_failed();
        weivel_memory_fail_at(0);
        bool whole = status == WEIVEL_STATUS_OK && answer.refusal.code == WEIVEL_REFUSAL_MALFORMED_MANIFEST &&
                     strstr(answer.refusal.detail, "twice") != NULL && answer.refusal.fix[0] != '\0';
        CHECK(whole || (status == WEIVEL_STATUS_OUT_OF_MEMORY && answer.storage == NULL));
        weivel_support_release(&answer);
        if (!failed) {
            CHECK(whole);
            break;
        }
        points++;
    }
    weivel_core_destroy(made.core);
    printf("supports of a malformed manifest: %u allocations, each failing whole or not at all\n", (unsigned)points);
}

static void world_with_a_served_question(world *made) {
    world_with_registration(made);
    weivel_want_options options = WEIVEL_INIT(weivel_want_options);
    options.poll_millis = 100;
    CHECK(weivel_want_service(made->core, made->picky, "calendar:events", &options, &made->want, NULL) ==
          WEIVEL_STATUS_OK);
    weivel_capacity answer = WEIVEL_INIT(weivel_capacity);
    answer.request = 1;
    weivel_number live = {(uint32_t)sizeof(weivel_number), "replicas", 5};
    answer.attributes.items = &live;
    answer.attributes.count = 1;
    CHECK(weivel_report_capacity(made->core, made->want, made->registration, &answer, 0, NULL) == WEIVEL_STATUS_OK);
}

static weivel_status picky_is_served(world *made) {
    weivel_capacity serves = WEIVEL_INIT(weivel_capacity);
    serves.request = 2;
    serves.serves = WEIVEL_CHOICE_YES;
    return weivel_report_capacity(made->core, made->want, made->registration, &serves, 0, NULL);
}

int main(void) {
    sweep("register a manifest", make_world, register_a_manifest);
    sweep("register a malformed manifest", make_world, register_a_malformed_manifest);
    supports_answers_a_whole_refusal_or_none();
    sweep("get a registered service", world_with_registration, notes_get);
    sweep("want a registered service", world_with_registration, notes_want);
    sweep("want, timed, with nothing registered", make_world, notes_want_timed);
    sweep("want, constrained, which asks its provider", world_with_registration, picky_want);
    sweep("want bound to another provider", world_with_registration, agenda_want_bound_elsewhere);
    sweep("watch a service", make_world, board_watch);
    sweep("watch a capability", make_world, board_watch_screens);
    sweep("register before waiting wants and watches", world_awaiting_events, calendar_registers);
    sweep("register again as the host, over a holder and a watch", world_with_a_host_clock,
          host_registers_clock_again);
    sweep("unregister with holders and a watch", world_with_holders, unregister_events);
    sweep("all of two verified implementations", world_with_two_verified_screens, notes_all);
    sweep("verify before two capability watches", world_watching_screens, calendar_verifies);
    sweep("provide a capability", make_world, calendar_provides);
    sweep("release with holders, watches and wants", world_at_full_stretch, release_calendar);
    sweep("quarantine with holders, watches and wants", world_at_full_stretch, quarantine_calendar);
    sweep("forget a released plugin that still holds", world_with_a_late_holder, forget_notes);
    sweep("get as the host", world_with_registration, host_gets);
    sweep("want as the host", make_world, host_wants);
    sweep("watch a service as the host", make_world, host_watches);
    sweep("watch a capability as the host", world_watching_screens, host_watches_screens);
    sweep("all as the host", world_with_two_verified_screens, host_all);
    sweep("a yes that settles a constrained want", world_with_a_served_question, picky_is_served);
    return check_summary("oom_test");
}
