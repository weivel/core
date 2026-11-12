#ifndef WEIVEL_REGISTRY_H
#define WEIVEL_REGISTRY_H

#include <stdbool.h>
#include <weivel/core.h>

#include "list.h"
#include "plugins.h"
#include "table.h"

struct weivel_registration;

/** Everything the registry knows about one service id: what is registered under it, who waits and who watches. */
typedef struct weivel_slot {
    /** Points just past the slot, in the same allocation. */
    char *id;
    struct weivel_registration *registration;
    weivel_link waiters;
    weivel_link watchers;
    weivel_link in_registry;
} weivel_slot;

/** One registered service. The core holds no implementation: `handle` stands for it. */
typedef struct weivel_registration {
    weivel_handle handle;
    weivel_slot *slot;
    /** NULL for a service the host registered. */
    weivel_plugin *owner;
    weivel_link by_owner;
    weivel_link holdings;
} weivel_registration;

typedef struct weivel_holding_key {
    weivel_handle registration;
    weivel_handle plugin;
} weivel_holding_key;

/** That `plugin` holds `registration`: it was handed its handle, and so is told when it is revoked. */
typedef struct weivel_holding {
    weivel_holding_key key;
    weivel_registration *registration;
    weivel_plugin *plugin;
    weivel_link by_registration;
    weivel_link by_plugin;
} weivel_holding;

/** A want still waiting. `provider` points into the consumer's manifest, which outlives it. */
typedef struct weivel_want {
    weivel_handle handle;
    weivel_slot *slot;
    weivel_plugin *consumer;
    const char *provider;
    bool timed;
    int64_t timeout_millis;
    weivel_link by_slot;
    weivel_link by_consumer;
} weivel_want;

typedef struct weivel_watch {
    weivel_handle handle;
    weivel_slot *slot;
    weivel_plugin *consumer;
    const char *provider;
    weivel_link by_slot;
    weivel_link by_consumer;
} weivel_watch;

/** One reported refusal of reach, the shape the ledger exposes. */
typedef struct weivel_reach_record {
    uint32_t kind;
    weivel_handle plugin;
    char *id;
} weivel_reach_record;

typedef struct weivel_registry {
    /** From each service id ever named to its slot, kept for the life of the core. */
    weivel_table *slots;
    weivel_link all_slots;
    /** From a weivel_holding_key to its holding, so holding a service twice is found without a scan. */
    weivel_table *holdings;
    weivel_reach_record *reported;
    uint32_t reported_count;
    uint32_t reported_capacity;
} weivel_registry;

weivel_registry *weivel_registry_create(void);

/** Frees every slot, registration, holding, want, watch and record, never a plugin. */
void weivel_registry_destroy(weivel_registry *registry);

/** The slot of `id`, or NULL when the id was never named. */
weivel_slot *weivel_registry_find(const weivel_registry *registry, const char *id);

/** The slot of `id`, made when the id was never named; NULL when memory runs out. */
weivel_slot *weivel_registry_slot(weivel_registry *registry, const char *id);

/** The identity of a registration's owner, or "the host". */
const char *weivel_registry_owner_name(const weivel_registration *registration);

/** Whether a consumer bound to `provider`, NULL for any, accepts what `registration`'s owner registered. */
bool weivel_registry_accepts(const char *provider, const weivel_registration *registration);

/** Records that `plugin` holds `registration`, once however often it is handed it. */
weivel_status weivel_registry_hold(weivel_core *core, weivel_registration *registration, weivel_plugin *plugin);

/** Ends a registration: revokes it to every holder, then tells every watch of its id that accepts its owner. */
void weivel_registry_unregister(weivel_core *core, weivel_registration *registration);

/**
 * Takes everything from a plugin in one go: its watches stop, what it registered is unregistered, every want it
 * waits on is rejected as stopped, and what it holds is revoked to it, in that order.
 */
void weivel_registry_strip(weivel_core *core, weivel_plugin *plugin);

/** How a plugin's attempt on a service id fared at its fence. */
typedef struct weivel_reach {
    /** WEIVEL_REFUSAL_NONE when the attempt may go on, else UNDECLARED or WRONG_PROVIDER. */
    uint32_t code;
    /** Whether the plugin is no longer running; read only when `code` is NONE. */
    bool late;
    /** The provider the plugin binds the id to, NULL for any. */
    const char *provider;
    char detail[384];
    char fix[384];
} weivel_reach;

/**
 * Checks a plugin's attempt on `id`: the declaration first, then a provider mismatch against the owner registered
 * now, then lateness. A refusal is reported once per plugin and id.
 */
void weivel_reach_check(weivel_core *core, weivel_plugin *plugin, const char *id, weivel_reach *reach);

/** Fills and reports, once per plugin and id, the refusal of a consumer bound to `provider` that `owner` serves. */
void weivel_reach_refuse_provider(weivel_core *core, weivel_plugin *plugin, const char *id, const char *provider,
                                  const char *owner, weivel_reach *reach);

/** Logs a late attempt (`what` is "want", "watch" or "registration") from a plugin no longer running. */
void weivel_reach_log_late(weivel_core *core, const weivel_plugin *plugin, const char *what, const char *id);

/** The texts a want gets when its plugin stops while it waits, or asks after it stopped. */
void weivel_reach_stopped(const char *id, weivel_reach *reach);

/** How many refusals of reach were reported, and the one at `index`; read with the core's mutex held. */
uint32_t weivel_reach_reported_count(const weivel_core *core);
const weivel_reach_record *weivel_reach_reported_at(const weivel_core *core, uint32_t index);

/** Settles a want with a registration, which its consumer then holds. */
void weivel_waiting_settle(weivel_core *core, weivel_want *want, weivel_registration *registration);

/** Rejects a want with a refusal. */
void weivel_waiting_reject(weivel_core *core, weivel_want *want, uint32_t code, const char *detail, const char *fix);

/** Withdraws a want without settling it. */
void weivel_waiting_withdraw(weivel_core *core, weivel_want *want);

/** Settles, in the order they asked, every want on a slot that accepts what is registered there now. */
void weivel_waiting_arrival(weivel_core *core, weivel_slot *slot);

/** Rejects the want a deadline named, when it still waits. */
void weivel_waiting_time_out(weivel_core *core, weivel_handle subject);

/** Frees a want's record without an event or a deadline, for destroy. */
void weivel_waiting_free(weivel_want *want);

#endif
