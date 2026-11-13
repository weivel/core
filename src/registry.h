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
    /** The constrained wants on the id, which settle only on their polls, so an arrival never walks them. */
    weivel_link pollers;
    weivel_link watchers;
    weivel_link in_registry;
} weivel_slot;

/** One registered service. The core holds no implementation: `handle` stands for it. */
typedef struct weivel_registration {
    weivel_handle handle;
    weivel_slot *slot;
    /** NULL for a service the host registered. */
    weivel_plugin *owner;
    /** What the owner's manifest advertises for the id, pointing into it; NULL when it advertises nothing. */
    const weivel_numbers *advertised;
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

struct weivel_constrained;

/** A want still waiting. `provider` points into the consumer's manifest, which outlives it. */
typedef struct weivel_want {
    weivel_handle handle;
    weivel_slot *slot;
    weivel_plugin *consumer;
    const char *provider;
    /** The poll state of a want whose declaration carries constraints, NULL for any other. */
    struct weivel_constrained *constrained;
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

typedef struct weivel_registry {
    /** From each service id to its slot, freed when nothing is registered, waiting or watching there. */
    weivel_table *slots;
    weivel_link all_slots;
    /** From a weivel_holding_key to its holding, so holding a service twice is found without a scan. */
    weivel_table *holdings;
    uint32_t slot_count;
    uint32_t holding_count;
    /** The sequence the next reported refusal takes, ordering reports across plugins. */
    uint64_t next_report;
    /** The last request a capacity question was given; each question takes the next, so none repeats. */
    uint64_t last_request;
    /**
     * Holdings allocated ahead by weivel_registry_prepare_holdings, chained by `by_registration`, each with its
     * revocation already reserved, so recording a holding inside a prepared call cannot fail.
     */
    weivel_link spare_holdings;
    uint32_t spare_holding_count;
    uint32_t want_count;
    uint32_t watch_count;
} weivel_registry;

weivel_registry *weivel_registry_create(void);

/** Frees every slot, registration, holding, want, watch and record, never a plugin. */
void weivel_registry_destroy(weivel_registry *registry);

/** The slot of `id`, or NULL when the id was never named. */
weivel_slot *weivel_registry_find(const weivel_registry *registry, const char *id);

/** The slot of `id`, made when none exists; NULL when memory runs out. */
weivel_slot *weivel_registry_slot(weivel_registry *registry, const char *id);

/** Frees a slot nothing is registered, waiting or watching on any more; a slot still in use is kept. */
void weivel_registry_release_slot(weivel_registry *registry, weivel_slot *slot);

/** The identity of a registration's owner, or "the host". */
const char *weivel_registry_owner_name(const weivel_registration *registration);

/** Whether a consumer bound to `provider`, NULL for any, accepts what `registration`'s owner registered. */
bool weivel_registry_accepts(const char *provider, const weivel_registration *registration);

/**
 * Allocates `count` holdings ahead, each reserving the revocation it will owe, so the next `count` holdings this
 * call records cannot fail. On WEIVEL_STATUS_OUT_OF_MEMORY the call changes nothing; whatever was allocated ahead
 * is freed when the call ends.
 */
weivel_status weivel_registry_prepare_holdings(weivel_core *core, uint32_t count);

/** Frees the holdings allocated ahead that no holding used, and gives back their reservations. */
void weivel_registry_finish(weivel_core *core);

/**
 * Records that `plugin` holds `registration`, once however often it is handed it, using a holding allocated ahead
 * when there is one. On WEIVEL_STATUS_OUT_OF_MEMORY nothing is recorded, and the caller must not hand the plugin
 * the registration, since it would never be told of its revocation.
 */
weivel_status weivel_registry_hold(weivel_core *core, weivel_registration *registration, weivel_plugin *plugin);

/** How many watches of the slot's id there are: the most notifications one change to it can owe. */
uint32_t weivel_registry_watchers(const weivel_slot *slot);

/**
 * Ends a registration: revokes it to every holder, then tells every watch of its id that accepts its owner, each
 * notification from a reservation the caller made (weivel_core_reserve_notices).
 */
void weivel_registry_unregister(weivel_core *core, weivel_registration *registration);

/** How many notifications weivel_registry_strip can owe for the plugin, at most. */
uint32_t weivel_registry_strip_notices(const weivel_plugin *plugin);

/**
 * Takes everything from a plugin in one go: its watches stop, what it registered is unregistered, every want it
 * waits on is rejected as stopped, and what it holds is revoked to it, in that order. The caller has reserved
 * weivel_registry_strip_notices of notifications, so nothing in it can fail.
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
    /** The plugin's manifest entry consuming the id, NULL when it only provides it. */
    const weivel_service_want *wanted;
    /** The refusal's texts, each built at its exact length and owned until weivel_reach_dispose; NULL for none. */
    char *detail;
    char *fix;
} weivel_reach;

/** Frees a reach's texts. */
void weivel_reach_dispose(weivel_reach *reach);

/**
 * Checks a plugin's attempt on `id`: the declaration first, then a provider mismatch against the owner registered
 * now, then lateness. A refusal is reported once per plugin and id. The host reaches every id. On
 * WEIVEL_STATUS_OUT_OF_MEMORY a refusal could not be reported and nothing was recorded.
 */
weivel_status weivel_reach_check(weivel_core *core, weivel_plugin *plugin, const char *id, weivel_reach *reach);

/**
 * Fills and reports, once per plugin and id, the refusal of a consumer bound to `provider` that `owner` serves;
 * WEIVEL_STATUS_OUT_OF_MEMORY, with nothing recorded, when it cannot.
 */
weivel_status weivel_reach_refuse_provider(weivel_core *core, weivel_plugin *plugin, const char *id,
                                           const char *provider, const char *owner, weivel_reach *reach);

/**
 * Takes back the report the call now running made for the plugin, when its newest report is numbered `since` or
 * later, together with its event, which must be the newest queued.
 */
void weivel_reach_unreport_since(weivel_core *core, weivel_plugin *plugin, uint64_t since);

/** Logs a late attempt (`what` is "want", "watch" or "registration") from a plugin no longer running. */
void weivel_reach_log_late(weivel_core *core, const weivel_plugin *plugin, const char *what, const char *id);

/** Logs a late attempt on a capability (`what` is "provision" or "watch") from a plugin no longer running. */
void weivel_reach_log_late_capability(weivel_core *core, const weivel_plugin *plugin, const char *what,
                                      const char *id);

/**
 * Whether a plugin may reach capability `id`, which it consumes or provides: WEIVEL_STATUS_OK when it may,
 * WEIVEL_STATUS_REFUSED, the refusal reported once per plugin and id, when it may not, and
 * WEIVEL_STATUS_OUT_OF_MEMORY when the report could not be made. Lateness is not looked at, so the declaration is
 * always checked first. The host reaches every id.
 */
weivel_status weivel_reach_capability(weivel_core *core, weivel_plugin *plugin, const char *id);

/** The texts a want gets when its plugin stops while it waits, or asks after it stopped. */
void weivel_reach_stopped(const char *id, weivel_reach *reach);

/** How many refusals of reach were reported for a plugin, and the one at `index`; read with the mutex held. */
uint32_t weivel_reach_reported_count(const weivel_plugin *plugin);
const weivel_reach_record *weivel_reach_reported_at(const weivel_plugin *plugin, uint32_t index);

/**
 * Settles a want with a registration, which its consumer then holds; the settlement is the want's own reservation.
 * `degraded_on` is NULL, or for a constrained want's acceptance of a provider that never met its constraints, the
 * constraint the settlement names. On WEIVEL_STATUS_OUT_OF_MEMORY the holding could not be recorded, so the want is
 * left exactly as it was, still waiting.
 */
weivel_status weivel_waiting_settle(weivel_core *core, weivel_want *want, weivel_registration *registration,
                                    const char *degraded_on);

/** Rejects a want with a refusal about `subject`, delivered from the want's own reservation. */
void weivel_waiting_reject(weivel_core *core, weivel_want *want, uint32_t code, const char *subject,
                           const char *detail, const char *fix);

/** Withdraws a want without settling it, giving back its reservation. */
void weivel_waiting_withdraw(weivel_core *core, weivel_want *want);

/** How many wants wait on a slot without constraints: the most holdings one arrival there can make. */
uint32_t weivel_waiting_count(const weivel_slot *slot);

/**
 * Reports, once per plugin, every want on the slot bound to a provider other than the owner of `registration`, as
 * its arrival will find them. All or nothing: on WEIVEL_STATUS_OUT_OF_MEMORY every report this made is taken back.
 */
weivel_status weivel_waiting_report_mismatches(weivel_core *core, const weivel_slot *slot,
                                               const weivel_registration *registration);

/**
 * Settles, in the order they asked, every want on a slot that accepts what is registered there now, from holdings
 * the caller prepared.
 */
void weivel_waiting_arrival(weivel_core *core, weivel_slot *slot);

/**
 * Acts on a deadline that is due at `now`: a timed want is rejected, a constrained one moves to its next poll or
 * question. Either way its deadline is replaced or removed, so each call moves the want on, and a deadline whose
 * want is gone is removed.
 */
void weivel_waiting_due(weivel_core *core, weivel_handle subject, int64_t now);

/** Frees a want's record without an event or a deadline, for destroy. */
void weivel_waiting_free(weivel_want *want);

#endif
