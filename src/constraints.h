#ifndef WEIVEL_CONSTRAINTS_H
#define WEIVEL_CONSTRAINTS_H

#include <stdbool.h>
#include <weivel/core.h>

#include "registry.h"
#include "table.h"

/** A host's constraint vocabulary, copied at create: every entry at today's size, its names in `storage`. */
typedef struct weivel_vocabulary {
    weivel_constraint_entry *entries;
    uint32_t count;
    /** From each constraint name to its entry. */
    weivel_table *by_name;
    void *storage;
} weivel_vocabulary;

/** Whether a caller's vocabulary can be read: entries of one size, every name given, a bound, no name twice. */
bool weivel_vocabulary_readable(const weivel_constraint_vocabulary *vocabulary);

/** Copies a readable vocabulary; false when memory runs out, with nothing left to free. */
bool weivel_vocabulary_copy(const weivel_constraint_vocabulary *source, weivel_vocabulary *copy);

void weivel_vocabulary_free(weivel_vocabulary *vocabulary);

/** Whether a want on this declaration is constrained, and so settles only on a poll. */
bool weivel_constraints_apply(const weivel_service_want *wanted);

/**
 * Starts a constrained want's polls: logs each constraint the vocabulary does not name, then runs poll 0 at
 * `options->now`. The want is on its slot's and consumer's lists already. On WEIVEL_STATUS_OUT_OF_MEMORY the want
 * is left for the caller to withdraw; on OK it may already have been rejected and freed.
 */
weivel_status weivel_constraints_start(weivel_core *core, weivel_want *want, const weivel_service_want *wanted,
                                       const weivel_want_options *options);

/** Acts on the want's deadline, due at `now`: the poll or question it marks has ended unanswered. */
void weivel_constraints_due(weivel_core *core, weivel_want *want, int64_t now);

/**
 * Counts a report against the want's open question. WEIVEL_STATUS_NONE when it answers none; `capacity` has been
 * checked readable.
 */
weivel_status weivel_constraints_report(weivel_core *core, weivel_want *want, weivel_handle registration,
                                        const weivel_capacity *capacity, int64_t now);

/** Frees a want's poll state, its deadline already cancelled. NULL is a no-op. */
void weivel_constraints_free(struct weivel_constrained *constrained);

#endif
