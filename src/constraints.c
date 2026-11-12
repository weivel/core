#include "constraints.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "manifest.h"
#include "sized.h"

#define WEIVEL_CONSTRAINT_ENTRY_FIRST_SIZE (offsetof(weivel_constraint_entry, bound) + sizeof(uint32_t))

typedef enum weivel_poll_phase {
    /** A poll's attributes question is open. */
    PHASE_ATTRIBUTES = 1,
    /** The poll's attributes met the constraints, and whether the provider serves under them is open. */
    PHASE_SERVE = 2,
    /** The poll has nothing open and waits for the next one. */
    PHASE_RESTING = 3,
    /** The budget is spent; whether the provider serves under the constraints after all is open. */
    PHASE_DEGRADE_CONSTRAINED = 4,
    /** The budget is spent; whether the provider serves an unconstrained ask is open. */
    PHASE_DEGRADE_UNCONSTRAINED = 5
} weivel_poll_phase;

/** Attributes as the core keeps them: today's size, the names copied into the same allocation. */
typedef struct weivel_kept_numbers {
    weivel_number *items;
    uint32_t count;
} weivel_kept_numbers;

/**
 * A constrained want's polls. `request` is the open question, 0 when none is open, and `closes` when the open poll
 * or question ends; the want's deadline is set at `closes` from its start until it retires, so moving it never
 * needs memory.
 */
typedef struct weivel_constrained {
    const weivel_service_want *wanted;
    int64_t start;
    int64_t budget;
    int64_t poll;
    int64_t poll_index;
    int64_t last_poll;
    uint32_t phase;
    uint64_t request;
    weivel_handle asked;
    int64_t closes;
    /** What the latest answered poll saw, advertised claims overlaid with live attributes; for texts only. */
    weivel_kept_numbers seen;
    bool answered;
} weivel_constrained;

typedef struct weivel_text {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} weivel_text;

static void append(weivel_text *text, const char *format, ...) {
    if (text->failed) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    va_list measuring;
    va_copy(measuring, arguments);
    int needed = vsnprintf(NULL, 0, format, measuring);
    va_end(measuring);
    if (needed < 0) {
        text->failed = true;
        va_end(arguments);
        return;
    }
    size_t wanted = text->length + (size_t)needed + 1;
    if (wanted > text->capacity) {
        size_t capacity = text->capacity == 0 ? 128 : text->capacity;
        while (capacity < wanted) {
            capacity *= 2;
        }
        char *grown = realloc(text->data, capacity);
        if (grown == NULL) {
            text->failed = true;
            va_end(arguments);
            return;
        }
        text->data = grown;
        text->capacity = capacity;
    }
    vsnprintf(text->data + text->length, text->capacity - text->length, format, arguments);
    text->length += (size_t)needed;
    va_end(arguments);
}

static const char *text_of(const weivel_text *text) {
    return text->failed || text->data == NULL ? "" : text->data;
}

static void describe(weivel_text *text, const weivel_number *items, uint32_t count) {
    if (count == 0) {
        append(text, "nothing");
    }
    for (uint32_t index = 0; index < count; index++) {
        append(text, "%s%s %g", index == 0 ? "" : ", ", items[index].name, items[index].value);
    }
}

static void describe_numbers(weivel_text *text, const weivel_numbers *numbers) {
    describe(text, numbers == NULL ? NULL : numbers->items, numbers == NULL ? 0 : numbers->count);
}

bool weivel_vocabulary_readable(const weivel_constraint_vocabulary *vocabulary) {
    if (!weivel_elements_readable(vocabulary->items, vocabulary->count, WEIVEL_CONSTRAINT_ENTRY_FIRST_SIZE)) {
        return false;
    }
    for (uint32_t index = 0; index < vocabulary->count; index++) {
        weivel_constraint_entry entry;
        weivel_element_read(vocabulary->items, vocabulary->count, index, &entry, sizeof entry);
        bool named = entry.constraint_name != NULL && entry.constraint_name[0] != '\0' &&
                     entry.attribute_name != NULL && entry.attribute_name[0] != '\0';
        if (!named || (entry.bound != WEIVEL_BOUND_LOWER && entry.bound != WEIVEL_BOUND_UPPER)) {
            return false;
        }
        for (uint32_t earlier = 0; earlier < index; earlier++) {
            weivel_constraint_entry before;
            weivel_element_read(vocabulary->items, vocabulary->count, earlier, &before, sizeof before);
            if (strcmp(before.constraint_name, entry.constraint_name) == 0) {
                return false;
            }
        }
    }
    return true;
}

static size_t vocabulary_bytes(const weivel_constraint_vocabulary *source) {
    size_t total = (size_t)source->count * sizeof(weivel_constraint_entry);
    for (uint32_t index = 0; index < source->count; index++) {
        weivel_constraint_entry entry;
        weivel_element_read(source->items, source->count, index, &entry, sizeof entry);
        total += strlen(entry.constraint_name) + strlen(entry.attribute_name) + 2;
    }
    return total == 0 ? 1 : total;
}

static char *keep(char **cursor, const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = *cursor;
    memcpy(copy, text, length);
    *cursor += length;
    return copy;
}

bool weivel_vocabulary_copy(const weivel_constraint_vocabulary *source, weivel_vocabulary *copy) {
    memset(copy, 0, sizeof *copy);
    copy->storage = malloc(vocabulary_bytes(source));
    copy->by_name = weivel_table_create();
    if (copy->storage == NULL || copy->by_name == NULL) {
        weivel_vocabulary_free(copy);
        return false;
    }
    copy->entries = copy->storage;
    char *cursor = (char *)copy->storage + (size_t)source->count * sizeof(weivel_constraint_entry);
    for (uint32_t index = 0; index < source->count; index++) {
        weivel_constraint_entry *entry = &copy->entries[index];
        weivel_element_read(source->items, source->count, index, entry, sizeof *entry);
        entry->size = (uint32_t)sizeof *entry;
        entry->constraint_name = keep(&cursor, entry->constraint_name);
        entry->attribute_name = keep(&cursor, entry->attribute_name);
        copy->count++;
        if (weivel_table_add_text(copy->by_name, entry->constraint_name, entry) != WEIVEL_STATUS_OK) {
            weivel_vocabulary_free(copy);
            return false;
        }
    }
    return true;
}

void weivel_vocabulary_free(weivel_vocabulary *vocabulary) {
    weivel_table_destroy(vocabulary->by_name);
    free(vocabulary->storage);
    memset(vocabulary, 0, sizeof *vocabulary);
}

bool weivel_constraints_apply(const weivel_service_want *wanted) {
    return wanted != NULL && wanted->constraints.count > 0;
}

void weivel_constraints_free(weivel_constrained *constrained) {
    if (constrained != NULL) {
        free(constrained->seen.items);
        free(constrained);
    }
}

static int64_t add_saturated(int64_t base, int64_t amount) {
    return base > INT64_MAX - amount ? INT64_MAX : base + amount;
}

static int64_t poll_start(const weivel_constrained *constrained, int64_t index) {
    if (index > INT64_MAX / constrained->poll) {
        return INT64_MAX;
    }
    return add_saturated(constrained->start, index * constrained->poll);
}

/** The poll whose time `now` falls in, so a late advance skips every poll that has wholly passed. */
static int64_t poll_at(const weivel_constrained *constrained, int64_t now) {
    if (now <= constrained->start) {
        return 0;
    }
    bool spans_too_far = constrained->start < 0 && now > INT64_MAX + constrained->start;
    int64_t elapsed = spans_too_far ? INT64_MAX : now - constrained->start;
    return elapsed / constrained->poll;
}

static bool is_last_poll(const weivel_constrained *constrained) {
    return constrained->poll_index >= constrained->last_poll;
}

/** The value `name` has among kept numbers, or NULL. */
static const weivel_number *kept_named(const weivel_kept_numbers *numbers, const char *name) {
    for (uint32_t index = 0; index < numbers->count; index++) {
        if (strcmp(numbers->items[index].name, name) == 0) {
            return &numbers->items[index];
        }
    }
    return NULL;
}

/**
 * Lays live attributes over advertised ones, name by name, into one allocation.
 * @implNote Per name, never one set or the other: a provider that reports only the attribute that changes live
 * would otherwise drop every claim its manifest made.
 */
static bool overlay(const weivel_numbers *advertised, const weivel_numbers *live, weivel_kept_numbers *seen) {
    uint32_t advertised_count = advertised == NULL ? 0 : advertised->count;
    size_t bytes = ((size_t)advertised_count + live->count) * sizeof(weivel_number);
    for (uint32_t index = 0; index < advertised_count; index++) {
        bytes += strlen(advertised->items[index].name) + 1;
    }
    for (uint32_t index = 0; index < live->count; index++) {
        weivel_number number;
        weivel_element_read(live->items, live->count, index, &number, sizeof number);
        bytes += strlen(number.name) + 1;
    }
    weivel_number *items = malloc(bytes == 0 ? 1 : bytes);
    if (items == NULL) {
        return false;
    }
    char *cursor = (char *)items + ((size_t)advertised_count + live->count) * sizeof(weivel_number);
    weivel_kept_numbers merged = {items, 0};
    for (uint32_t index = 0; index < advertised_count; index++) {
        weivel_number *kept = &items[merged.count++];
        *kept = advertised->items[index];
        kept->name = keep(&cursor, kept->name);
    }
    for (uint32_t index = 0; index < live->count; index++) {
        weivel_number number;
        weivel_element_read(live->items, live->count, index, &number, sizeof number);
        weivel_number *existing = (weivel_number *)kept_named(&merged, number.name);
        if (existing != NULL) {
            existing->value = number.value;
            continue;
        }
        weivel_number *kept = &items[merged.count++];
        kept->size = (uint32_t)sizeof *kept;
        kept->name = keep(&cursor, number.name);
        kept->value = number.value;
    }
    *seen = merged;
    return true;
}

/**
 * Whether `seen` meets every constraint.
 * @implNote A constraint the vocabulary does not name is NOT met: reading it as met would let a typo in a manifest
 * read as a requirement nothing has to meet.
 */
static bool satisfies(const weivel_vocabulary *vocabulary, const weivel_numbers *constraints,
                      const weivel_kept_numbers *seen) {
    for (uint32_t index = 0; index < constraints->count; index++) {
        const weivel_number *constraint = &constraints->items[index];
        const weivel_constraint_entry *entry = weivel_table_find_text(vocabulary->by_name, constraint->name);
        const weivel_number *offered = entry == NULL ? NULL : kept_named(seen, entry->attribute_name);
        if (offered == NULL) {
            return false;
        }
        bool met = entry->bound == WEIVEL_BOUND_UPPER ? offered->value <= constraint->value
                                                       : offered->value >= constraint->value;
        if (!met) {
            return false;
        }
    }
    return true;
}

static void log_unknown(weivel_core *core, const weivel_want *want) {
    const weivel_numbers *constraints = &want->constrained->wanted->constraints;
    for (uint32_t index = 0; index < constraints->count; index++) {
        const char *name = constraints->items[index].name;
        if (weivel_table_find_text(core->vocabulary.by_name, name) != NULL) {
            continue;
        }
        weivel_text text = {NULL, 0, 0, false};
        append(&text, "constraint \"%s\" is in no vocabulary this host declared, so nothing can satisfy it for %s",
               name, want->consumer->identity);
        weivel_core_log(core, WEIVEL_LOG_WARNING, text_of(&text));
        free(text.data);
    }
}

/** The registration the want may poll now, reporting once a provider it does not accept. */
static weivel_registration *candidate(weivel_core *core, weivel_want *want) {
    weivel_registration *registration = want->slot->registration;
    if (registration == NULL) {
        return NULL;
    }
    if (!weivel_registry_accepts(want->provider, registration)) {
        weivel_reach mismatch;
        weivel_reach_refuse_provider(core, want->consumer, want->slot->id, want->provider,
                                     weivel_registry_owner_name(registration), &mismatch);
        return NULL;
    }
    return registration;
}

/** Moves the want's deadline to `closes`; only the first, at the want's start, can run out of memory. */
static weivel_status close_at(weivel_core *core, weivel_want *want, int64_t closes) {
    want->constrained->closes = closes;
    return weivel_deadlines_add(core->deadlines, closes, want->handle);
}

static weivel_status ask(weivel_core *core, weivel_want *want, weivel_registration *registration, uint32_t phase,
                         int64_t closes) {
    weivel_constrained *constrained = want->constrained;
    weivel_status status = close_at(core, want, closes);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    constrained->phase = phase;
    constrained->request = ++core->registry->last_request;
    constrained->asked = registration->handle;
    bool unconstrained = phase == PHASE_DEGRADE_UNCONSTRAINED;
    uint32_t question = phase == PHASE_ATTRIBUTES ? WEIVEL_CAPACITY_ATTRIBUTES : WEIVEL_CAPACITY_SERVE;
    weivel_capacity_event asked = {want->handle, want->consumer->handle, registration->handle, constrained->request,
                                   question, {NULL, 0}};
    weivel_events_push_capacity(core->events, &asked, unconstrained ? NULL : &constrained->wanted->constraints);
    return WEIVEL_STATUS_OK;
}

static void describe_offered(weivel_text *text, const weivel_constrained *constrained,
                             const weivel_registration *registration) {
    if (constrained->answered) {
        describe(text, constrained->seen.items, constrained->seen.count);
    } else {
        describe_numbers(text, registration->advertised);
    }
}

static void reject_unmet(weivel_core *core, weivel_want *want, const weivel_registration *registration) {
    const weivel_constrained *constrained = want->constrained;
    const weivel_numbers *constraints = &constrained->wanted->constraints;
    weivel_text detail = {NULL, 0, 0, false};
    append(&detail, "needs \"%s\" meeting ", want->slot->id);
    describe_numbers(&detail, constraints);
    append(&detail, ", and after %" PRId64 " ms ", constrained->budget);
    if (registration == NULL) {
        append(&detail, "nothing provides it");
    } else {
        append(&detail, "its provider still offers ");
        describe_offered(&detail, constrained, registration);
    }
    weivel_text fix = {NULL, 0, 0, false};
    append(&fix, "run a provider that meets ");
    describe_numbers(&fix, constraints);
    append(&fix, ", or mark the requirement degradable to accept what there is");
    weivel_waiting_reject(core, want, WEIVEL_REFUSAL_UNMET_CONSTRAINT, text_of(&detail), text_of(&fix));
    free(detail.data);
    free(fix.data);
}

static void reject_not_served(weivel_core *core, weivel_want *want) {
    weivel_text detail = {NULL, 0, 0, false};
    append(&detail, "needs \"%s\", and its provider would not serve even an unconstrained ask", want->slot->id);
    weivel_waiting_reject(core, want, WEIVEL_REFUSAL_NOT_SERVED, text_of(&detail),
                          "have the provider serve an unconstrained ask, or stop declaring the requirement degradable");
    free(detail.data);
}

/**
 * Spends the budget: refuses a want that does not degrade or has nothing to degrade to, else asks the provider
 * once more under the constraints.
 * @implNote The constraints are asked once more first, because the provider may have started meeting them in the
 * instant after the budget ran out.
 */
static void spend_budget(weivel_core *core, weivel_want *want, int64_t now) {
    weivel_registration *registration = candidate(core, want);
    if (want->constrained->wanted->degradable != WEIVEL_CHOICE_YES || registration == NULL) {
        reject_unmet(core, want, registration);
        return;
    }
    ask(core, want, registration, PHASE_DEGRADE_CONSTRAINED, add_saturated(now, want->constrained->poll));
}

static weivel_status begin_poll(weivel_core *core, weivel_want *want, int64_t index) {
    weivel_constrained *constrained = want->constrained;
    constrained->poll_index = index;
    constrained->phase = PHASE_RESTING;
    constrained->request = 0;
    int64_t closes = index == INT64_MAX ? INT64_MAX : poll_start(constrained, index + 1);
    weivel_registration *registration = candidate(core, want);
    if (registration != NULL) {
        return ask(core, want, registration, PHASE_ATTRIBUTES, closes);
    }
    if (is_last_poll(constrained)) {
        reject_unmet(core, want, NULL);
        return WEIVEL_STATUS_OK;
    }
    return close_at(core, want, closes);
}

static void degraded_report(weivel_core *core, const weivel_want *want, const weivel_registration *registration) {
    const weivel_constrained *constrained = want->constrained;
    weivel_text text = {NULL, 0, 0, false};
    append(&text, "%s accepted a degraded \"%s\": it asked for ", want->consumer->identity, want->slot->id);
    describe_numbers(&text, &constrained->wanted->constraints);
    append(&text, " and was offered ");
    describe_offered(&text, constrained, registration);
    weivel_core_log(core, WEIVEL_LOG_WARNING, text_of(&text));
    free(text.data);
}

/**
 * Settles the want with the provider that said yes. Degradation is never silent: what was accepted is reported
 * before it is handed over, once the holding is sure.
 */
static void serve(weivel_core *core, weivel_want *want, weivel_registration *registration, bool degraded) {
    if (degraded && weivel_registry_hold(core, registration, want->consumer) == WEIVEL_STATUS_OK) {
        degraded_report(core, want, registration);
    }
    if (weivel_waiting_settle(core, want, registration, degraded) != WEIVEL_STATUS_OK) {
        weivel_core_log(core, WEIVEL_LOG_ERROR, "memory ran out settling a want, so it still waits");
        if (want->constrained->phase == PHASE_SERVE) {
            want->constrained->phase = PHASE_RESTING;
        }
    }
}

/** Ends a poll with nothing to serve: the last one spends the budget, any other waits for the next poll. */
static void poll_failed(weivel_core *core, weivel_want *want, int64_t now) {
    if (is_last_poll(want->constrained)) {
        spend_budget(core, want, now);
    } else {
        want->constrained->phase = PHASE_RESTING;
    }
}

weivel_status weivel_constraints_start(weivel_core *core, weivel_want *want, const weivel_service_want *wanted,
                                       const weivel_want_options *options) {
    weivel_constrained *constrained = calloc(1, sizeof *constrained);
    if (constrained == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    bool budgeted = options->budgeted == WEIVEL_CHOICE_YES;
    constrained->wanted = wanted;
    constrained->start = options->now;
    constrained->budget = budgeted ? options->budget_millis : WEIVEL_DEFAULT_BUDGET_MILLIS;
    constrained->poll = budgeted ? options->poll_millis : WEIVEL_DEFAULT_POLL_MILLIS;
    constrained->last_poll =
        constrained->budget / constrained->poll + (constrained->budget % constrained->poll != 0 ? 1 : 0);
    want->constrained = constrained;
    log_unknown(core, want);
    return begin_poll(core, want, 0);
}

void weivel_constraints_due(weivel_core *core, weivel_want *want, int64_t now) {
    weivel_constrained *constrained = want->constrained;
    constrained->request = 0;
    if (constrained->phase == PHASE_DEGRADE_CONSTRAINED) {
        weivel_registration *registration = candidate(core, want);
        if (registration == NULL) {
            reject_unmet(core, want, NULL);
        } else {
            ask(core, want, registration, PHASE_DEGRADE_UNCONSTRAINED, add_saturated(now, constrained->poll));
        }
        return;
    }
    if (constrained->phase == PHASE_DEGRADE_UNCONSTRAINED) {
        reject_not_served(core, want);
        return;
    }
    if (is_last_poll(constrained)) {
        spend_budget(core, want, now);
        return;
    }
    int64_t next = poll_at(constrained, now);
    if (next <= constrained->poll_index) {
        next = constrained->poll_index + 1;
    }
    if (next > constrained->last_poll) {
        spend_budget(core, want, now);
        return;
    }
    begin_poll(core, want, next);
}

/** Whether a report answers the want's open question for the registration still under its id. */
static bool answers_open_question(const weivel_want *want, weivel_handle registration, const weivel_capacity *capacity,
                                  int64_t now) {
    const weivel_constrained *constrained = want->constrained;
    const weivel_registration *current = want->slot->registration;
    return constrained->request != 0 && capacity->request == constrained->request &&
           registration == constrained->asked && now < constrained->closes && current != NULL &&
           current->handle == registration;
}

static weivel_status answer_attributes(weivel_core *core, weivel_want *want, weivel_registration *registration,
                                       const weivel_capacity *capacity, int64_t now) {
    weivel_constrained *constrained = want->constrained;
    weivel_kept_numbers seen;
    if (!overlay(registration->advertised, &capacity->attributes, &seen)) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    constrained->request = 0;
    free(constrained->seen.items);
    constrained->seen = seen;
    constrained->answered = true;
    if (satisfies(&core->vocabulary, &constrained->wanted->constraints, &constrained->seen)) {
        ask(core, want, registration, PHASE_SERVE, constrained->closes);
    } else {
        poll_failed(core, want, now);
    }
    return WEIVEL_STATUS_OK;
}

weivel_status weivel_constraints_report(weivel_core *core, weivel_want *want, weivel_handle registration,
                                        const weivel_capacity *capacity, int64_t now) {
    if (want->constrained == NULL || !answers_open_question(want, registration, capacity, now)) {
        return WEIVEL_STATUS_NONE;
    }
    weivel_constrained *constrained = want->constrained;
    weivel_registration *current = want->slot->registration;
    if (constrained->phase == PHASE_ATTRIBUTES) {
        return answer_attributes(core, want, current, capacity, now);
    }
    constrained->request = 0;
    bool serves = capacity->serves == WEIVEL_CHOICE_YES;
    uint32_t phase = constrained->phase;
    if (serves) {
        serve(core, want, current, phase != PHASE_SERVE);
    } else if (phase == PHASE_SERVE) {
        poll_failed(core, want, now);
    } else if (phase == PHASE_DEGRADE_CONSTRAINED) {
        ask(core, want, current, PHASE_DEGRADE_UNCONSTRAINED, add_saturated(now, constrained->poll));
    } else {
        reject_not_served(core, want);
    }
    return WEIVEL_STATUS_OK;
}

static bool read_capacity(const weivel_capacity *capacity, weivel_capacity *resolved) {
    memset(resolved, 0, sizeof *resolved);
    if (capacity == NULL || weivel_sized_size(capacity) < WEIVEL_CAPACITY_FIRST_SIZE) {
        return false;
    }
    weivel_sized_read(resolved, sizeof *resolved, capacity);
    return weivel_numbers_readable(&resolved->attributes);
}

static weivel_status refuse_stale_want(weivel_core *core, weivel_handle handle) {
    weivel_text detail = {NULL, 0, 0, false};
    append(&detail, "the handle %" PRIu64 " names no want on this core", handle);
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_STALE_HANDLE, text_of(&detail),
                               "answer with the want a capacity event named, and only until that want settles");
    free(detail.data);
    return WEIVEL_STATUS_REFUSED;
}

static weivel_status report_capacity(weivel_core *core, weivel_handle want, weivel_handle registration,
                                     const weivel_capacity *capacity, int64_t now) {
    void *record;
    if (weivel_handles_resolve(core->handles, want, WEIVEL_RECORD_WANT, &record) != WEIVEL_STATUS_OK) {
        return refuse_stale_want(core, want);
    }
    return weivel_constraints_report(core, record, registration, capacity, now);
}

weivel_status weivel_report_capacity(weivel_core *core, weivel_handle want, weivel_handle registration,
                                     const weivel_capacity *capacity, int64_t now, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_capacity resolved;
    weivel_status status;
    if (!read_capacity(capacity, &resolved)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_report_capacity was given no capacity, one too small, or attributes that cannot be read or "
                  "name one twice",
            "pass a weivel_capacity initialised with WEIVEL_INIT, its attributes one array of named numbers, each "
            "name once");
    } else {
        status = report_capacity(core, want, registration, &resolved, now);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}
