#ifndef WEIVEL_LEDGER_H
#define WEIVEL_LEDGER_H

#include <stdbool.h>
#include <weivel/core.h>

#include "table.h"

struct weivel_plugin;

/** Ids in the order each was first added, each once, every one a copy the set owns. */
typedef struct weivel_ids {
    char **items;
    uint32_t count;
    uint32_t capacity;
    /** From each id to itself, keyed by the copy in `items`; made on the first add. */
    weivel_table *index;
} weivel_ids;

/** Frees every id and leaves the set empty. */
void weivel_ids_clear(weivel_ids *ids);

/** A refusal whose texts the core owns; code NONE with NULL texts when there is none. */
typedef struct weivel_kept_refusal {
    uint32_t code;
    char *detail;
    char *fix;
} weivel_kept_refusal;

void weivel_kept_refusal_clear(weivel_kept_refusal *refusal);

/** Whether a kept refusal holds one; its code may be NONE when the caller gave no code. */
bool weivel_kept_refusal_held(const weivel_kept_refusal *refusal);

/** Copies a refusal's texts into `kept`, which must be empty; false, with `kept` still empty, when memory runs out. */
bool weivel_kept_refusal_copy(weivel_kept_refusal *kept, uint32_t code, const char *detail, const char *fix);

/** A relationship the ledger records for a plugin. */
typedef enum weivel_relationship {
    WEIVEL_RELATIONSHIP_SERVICE_PROVIDED = 1,
    WEIVEL_RELATIONSHIP_CAPABILITY_PROVIDED = 2,
    WEIVEL_RELATIONSHIP_SERVICE_CONSUMED = 3
} weivel_relationship;

/**
 * Records that `plugin` made a relationship with `id`. Called with the core's mutex held.
 *
 * @implNote Recording follows the act it records and never undoes it: when memory runs out, the ledger misses the
 * relationship and an error log says so, which is cheaper to explain than a registration refused for bookkeeping.
 */
void weivel_ledger_note(weivel_core *core, struct weivel_plugin *plugin, uint32_t relationship, const char *id);

/** Starts the row's description of a new activation: no relationships, not verified, no activation refusal. */
void weivel_ledger_begin_activation(struct weivel_plugin *plugin);

/** Records that the plugin's activation passed verification. */
void weivel_ledger_note_verified(struct weivel_plugin *plugin);

/** Keeps the refusal verification answered the plugin's activation, logging an error when it cannot be copied. */
void weivel_ledger_note_refused_activation(weivel_core *core, struct weivel_plugin *plugin, uint32_t code,
                                           const char *detail, const char *fix);

/** Frees everything the ledger keeps for a plugin. */
void weivel_ledger_free_plugin(struct weivel_plugin *plugin);

#endif
