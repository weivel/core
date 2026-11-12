#ifndef WEIVEL_PLUGINS_H
#define WEIVEL_PLUGINS_H

#include <stdbool.h>
#include <weivel/core.h>

#include "ledger.h"
#include "list.h"
#include "manifest.h"
#include "table.h"

/** Where a plugin is in its life. Only an open plugin's calls are on time; a quarantined or released one's are late. */
typedef enum weivel_plugin_state {
    WEIVEL_PLUGIN_REGISTERED = 0,
    WEIVEL_PLUGIN_OPEN = 1,
    WEIVEL_PLUGIN_QUARANTINED = 2,
    WEIVEL_PLUGIN_RELEASED = 3
} weivel_plugin_state;

/** A service id a plugin may reach, and the provider identity it binds the id to, NULL for any provider. */
typedef struct weivel_declaration {
    const char *id;
    const char *provider;
    bool consumed;
    /** The manifest's entries for the id, NULL where it does not consume or does not provide it. */
    const weivel_service_want *wanted;
    const weivel_service_offer *offered;
} weivel_declaration;

/** One reported refusal of reach, the shape the ledger exposes; `sequence` orders reports across plugins. */
typedef struct weivel_reach_record {
    uint32_t kind;
    weivel_handle plugin;
    char *id;
    uint64_t sequence;
} weivel_reach_record;

/**
 * A registered plugin: the manifest it was registered with, owned by the core, the handle that names it, and what
 * it holds in the registry. A released plugin's record is kept, so its late calls still resolve, until it is
 * forgotten.
 */
typedef struct weivel_plugin {
    weivel_owned_manifest manifest;
    weivel_handle handle;
    /** Where the plugin sits in the collection, so forgetting it takes no search. */
    uint32_t index;
    uint32_t state;
    /** The manifest's identity, never NULL. */
    const char *identity;
    uint64_t payload;
    /** The services it consumes, then those it only provides, each id once, pointing into `manifest`. */
    weivel_declaration *declarations;
    uint32_t declaration_count;
    /** From each declared id to its declaration. */
    weivel_table *declared;
    /** The service ids whose refusal has been reported for this plugin, keyed by the id its reported record owns. */
    weivel_table *reported_services;
    weivel_reach_record *reported;
    uint32_t reported_count;
    uint32_t reported_capacity;
    weivel_link registrations;
    weivel_link wants;
    weivel_link watches;
    weivel_link holdings;
    /** The capability ids it may reach, consumed or provided, each mapped to itself, pointing into `manifest`. */
    weivel_table *capabilities_reachable;
    /** The capability ids its manifest provides, each mapped to itself, pointing into `manifest`. */
    weivel_table *capabilities_declared;
    /** From each capability id it provided to the provision, keyed by the capability's own id. */
    weivel_table *provided;
    /** The capability ids whose refusal has been reported for this plugin, keyed by the reported record's id. */
    weivel_table *reported_capabilities;
    /** Its provisions, in the order provided. */
    weivel_link provisions;
    weivel_link capability_watches;
    weivel_link capability_holdings;
    /** Its place among the plugins in the order they were registered, which the ledger's rows keep. */
    weivel_link in_order;
    /** What the ledger records of its current, or last, activation. */
    weivel_ids services_provided;
    weivel_ids capabilities_provided;
    weivel_ids services_consumed;
    bool verified;
    /** The refusal verification last answered its activation since it was opened. */
    weivel_kept_refusal activation_refusal;
} weivel_plugin;

/** Every plugin record one core owns; the handle table only borrows them. */
typedef struct weivel_plugins weivel_plugins;

weivel_plugins *weivel_plugins_create(void);

/** Frees the collection and every plugin in it, manifests and all. */
void weivel_plugins_destroy(weivel_plugins *plugins);

/** Whether a plugin that is not released holds `import_name`. */
bool weivel_plugins_named(const weivel_plugins *plugins, const char *import_name);

/** The plugin that is not released holding `identity`, or NULL. */
weivel_plugin *weivel_plugins_with_identity(const weivel_plugins *plugins, const char *identity);

/** How many plugin records the collection holds, released ones included until they are forgotten. */
uint32_t weivel_plugins_count(const weivel_plugins *plugins);

/** The head of the list of every plugin record, chained by `in_order` in the order they were registered. */
const weivel_link *weivel_plugins_in_order(const weivel_plugins *plugins);

#endif
