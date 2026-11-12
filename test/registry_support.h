#ifndef WEIVEL_TEST_REGISTRY_SUPPORT_H
#define WEIVEL_TEST_REGISTRY_SUPPORT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <weivel/core.h>

#include "check.h"

/** One drained event, copied so the journal outlives the event's release. */
typedef struct seen {
    uint32_t kind;
    weivel_handle subject;
    weivel_handle plugin;
    weivel_handle implementation;
    uint32_t code;
    uint32_t change;
    uint32_t level;
    char id[128];
    char detail[512];
    char fix[512];
} seen;

#define JOURNAL_CAPACITY 256

typedef struct journal {
    seen items[JOURNAL_CAPACITY];
    uint32_t count;
} journal;

static journal the_journal;

static inline void copy_text(char *into, size_t capacity, const char *text) {
    snprintf(into, capacity, "%s", text == NULL ? "" : text);
}

static inline void note_refusal(seen *item, const weivel_refusal *refusal) {
    item->code = refusal->code;
    copy_text(item->detail, sizeof item->detail, refusal->detail);
    copy_text(item->fix, sizeof item->fix, refusal->fix);
}

static inline void note(seen *item, const weivel_event *event) {
    memset(item, 0, sizeof *item);
    item->kind = event->kind;
    switch (event->kind) {
    case WEIVEL_EVENT_LOG:
        item->level = event->data.log.level;
        copy_text(item->detail, sizeof item->detail, event->data.log.text);
        break;
    case WEIVEL_EVENT_REFUSAL:
        note_refusal(item, &event->data.refusal);
        break;
    case WEIVEL_EVENT_WANT_SETTLED:
        item->subject = event->data.want.want;
        item->plugin = event->data.want.plugin;
        item->implementation = event->data.want.implementation;
        note_refusal(item, &event->data.want.refusal);
        break;
    case WEIVEL_EVENT_WATCH:
        item->subject = event->data.watch.watch;
        item->plugin = event->data.watch.plugin;
        item->implementation = event->data.watch.implementation;
        item->change = event->data.watch.change;
        break;
    case WEIVEL_EVENT_REVOKED:
        item->plugin = event->data.revoked.plugin;
        item->implementation = event->data.revoked.implementation;
        break;
    case WEIVEL_EVENT_REACH_REFUSED:
        item->plugin = event->data.reach.plugin;
        item->change = event->data.reach.kind;
        copy_text(item->id, sizeof item->id, event->data.reach.id);
        note_refusal(item, &event->data.reach.refusal);
        break;
    default:
        break;
    }
}

/** Drains every waiting event into the journal; past its capacity, events are still drained and released. */
static inline void drain(weivel_core *core) {
    weivel_event event = WEIVEL_INIT(weivel_event);
    while (weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK) {
        if (the_journal.count < JOURNAL_CAPACITY) {
            note(&the_journal.items[the_journal.count++], &event);
        }
        weivel_event_release(&event);
    }
}

static inline void forget(weivel_core *core) {
    drain(core);
    the_journal.count = 0;
}

static inline uint32_t count_kind(uint32_t kind) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < the_journal.count; index++) {
        count += the_journal.items[index].kind == kind;
    }
    return count;
}

static const seen no_event_seen = {0};

/** The index-th journal entry of `kind`, or an empty entry of kind NONE, so a missing event fails a check cleanly. */
static inline const seen *nth_of(uint32_t kind, uint32_t index) {
    for (uint32_t at = 0; at < the_journal.count; at++) {
        if (the_journal.items[at].kind == kind && index-- == 0) {
            return &the_journal.items[at];
        }
    }
    return &no_event_seen;
}

/** The settlement of a want, or NULL while it waits; a second settlement fails a check. */
static inline const seen *settlement_of(weivel_handle want) {
    const seen *found = NULL;
    for (uint32_t at = 0; at < the_journal.count; at++) {
        if (the_journal.items[at].kind == WEIVEL_EVENT_WANT_SETTLED && the_journal.items[at].subject == want) {
            CHECK(found == NULL);
            found = &the_journal.items[at];
        }
    }
    return found;
}

static inline uint32_t revocations_to(weivel_handle plugin, weivel_handle implementation) {
    uint32_t count = 0;
    for (uint32_t at = 0; at < the_journal.count; at++) {
        const seen *item = &the_journal.items[at];
        count += item->kind == WEIVEL_EVENT_REVOKED && item->plugin == plugin && item->implementation == implementation;
    }
    return count;
}

static inline uint32_t heard_by(weivel_handle watch, uint32_t change) {
    uint32_t count = 0;
    for (uint32_t at = 0; at < the_journal.count; at++) {
        const seen *item = &the_journal.items[at];
        count += item->kind == WEIVEL_EVENT_WATCH && item->subject == watch && item->change == change;
    }
    return count;
}

static inline int mentions(const char *text, const char *part) {
    if (strstr(text, part) != NULL) {
        return 1;
    }
    fprintf(stderr, "expected \"%s\" to mention \"%s\"\n", text, part);
    check_failures++;
    return 0;
}

/** A plugin as a reach vector describes it: consumed ids, each with an optional provider, and provided ids. */
typedef struct plugin_spec {
    const char *identity;
    const char *import_name;
    const char *consumes[4];
    const char *providers[4];
    const char *provides[4];
} plugin_spec;

static inline uint32_t count_of(const char *const *items, uint32_t capacity) {
    uint32_t count = 0;
    while (count < capacity && items[count] != NULL) {
        count++;
    }
    return count;
}

static inline weivel_handle register_spec(weivel_core *core, const plugin_spec *spec) {
    weivel_service_want wants[4];
    weivel_service_offer offers[4];
    uint32_t consumed = count_of(spec->consumes, 4);
    uint32_t provided = count_of(spec->provides, 4);
    for (uint32_t index = 0; index < consumed; index++) {
        weivel_service_want want = WEIVEL_INIT(weivel_service_want);
        want.id = spec->consumes[index];
        want.provider = spec->providers[index];
        wants[index] = want;
    }
    for (uint32_t index = 0; index < provided; index++) {
        weivel_service_offer offer = WEIVEL_INIT(weivel_service_offer);
        offer.id = spec->provides[index];
        offers[index] = offer;
    }
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = spec->import_name != NULL ? spec->import_name : spec->identity;
    manifest.identity = spec->identity;
    manifest.api = 1;
    manifest.services.consumes.items = consumed == 0 ? NULL : wants;
    manifest.services.consumes.count = consumed;
    manifest.services.provides.items = provided == 0 ? NULL : offers;
    manifest.services.provides.count = provided;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

/** Registers a plugin under its identity and opens it, as a host opens a plugin's context. */
static inline weivel_handle open_spec(weivel_core *core, const plugin_spec *spec) {
    weivel_handle plugin = register_spec(core, spec);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    return plugin;
}

static inline weivel_core *core_knowing(const char *const *well_known, uint32_t count) {
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.well_known_services.items = well_known;
    options.well_known_services.count = count;
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    the_journal.count = 0;
    return core;
}

static inline weivel_handle register_service(weivel_core *core, weivel_handle owner, const char *id) {
    weivel_handle registration = 0;
    CHECK(weivel_register_service(core, owner, id, &registration, NULL) == WEIVEL_STATUS_OK);
    CHECK(registration != 0);
    return registration;
}

static inline weivel_handle want_untimed(weivel_core *core, weivel_handle plugin, const char *id) {
    weivel_handle want = 0;
    CHECK(weivel_want_service(core, plugin, id, NULL, &want, NULL) == WEIVEL_STATUS_OK);
    CHECK(want != 0);
    return want;
}

static inline weivel_handle want_within(weivel_core *core, weivel_handle plugin, const char *id, int64_t now,
                                 int64_t timeout) {
    weivel_want_options options = WEIVEL_INIT(weivel_want_options);
    options.timed = WEIVEL_CHOICE_YES;
    options.now = now;
    options.timeout_millis = timeout;
    weivel_handle want = 0;
    CHECK(weivel_want_service(core, plugin, id, &options, &want, NULL) == WEIVEL_STATUS_OK);
    CHECK(want != 0);
    return want;
}

#endif
