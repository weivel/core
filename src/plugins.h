#ifndef WEIVEL_PLUGINS_H
#define WEIVEL_PLUGINS_H

#include <stdbool.h>
#include <weivel/core.h>

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
} weivel_declaration;

/**
 * A registered plugin: the manifest it was registered with, owned by the core, the handle that names it, and what
 * it holds in the registry. A released plugin's record is kept, so its late calls still resolve, until the core
 * is destroyed.
 */
typedef struct weivel_plugin {
    weivel_owned_manifest manifest;
    weivel_handle handle;
    uint32_t state;
    /** NULL until the plugin is first opened. */
    char *identity;
    uint64_t payload;
    /** The services it consumes, then those it only provides, each id once, pointing into `manifest`. */
    weivel_declaration *declarations;
    uint32_t declaration_count;
    /** From each declared id to its declaration. */
    weivel_table *declared;
    /** The service ids whose refusal has been reported for this plugin, keyed by the id its reported record owns. */
    weivel_table *reported_services;
    weivel_link registrations;
    weivel_link wants;
    weivel_link watches;
    weivel_link holdings;
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

#endif
