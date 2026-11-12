#ifndef WEIVEL_CAPABILITIES_H
#define WEIVEL_CAPABILITIES_H

#include <stdbool.h>
#include <weivel/core.h>

#include "list.h"
#include "plugins.h"
#include "table.h"

/** Everything known about one capability id: who provided it, which of those are verified, and who watches it. */
typedef struct weivel_capability {
    /** Points just past the record, in the same allocation. */
    char *id;
    /** Its provisions, in the order provided, verified or not. */
    weivel_link provided;
    /** Its verified provisions, in the order their plugins' activations were verified: what `all` answers. */
    weivel_link verified;
    weivel_link watchers;
    weivel_link in_capabilities;
} weivel_capability;

/** One implementation a plugin provided. The core holds no object: `handle` stands for it. */
typedef struct weivel_provision {
    weivel_handle handle;
    weivel_capability *capability;
    weivel_plugin *provider;
    bool verified;
    weivel_link by_capability;
    weivel_link in_verified;
    weivel_link by_provider;
} weivel_provision;

typedef struct weivel_capability_watch {
    weivel_handle handle;
    weivel_capability *capability;
    weivel_plugin *consumer;
    weivel_link by_capability;
    weivel_link by_consumer;
} weivel_capability_watch;

typedef struct weivel_capabilities {
    /** From each capability id to its record, freed when nothing provides or watches it. */
    weivel_table *by_id;
    weivel_link all;
    uint32_t count;
    uint32_t provision_count;
    uint32_t watch_count;
} weivel_capabilities;

weivel_capabilities *weivel_capabilities_create(void);

/** Frees every capability, provision and watch, never a plugin. */
void weivel_capabilities_destroy(weivel_capabilities *capabilities);

/**
 * Takes every capability from a plugin: each verified implementation it provided is withdrawn and every watch of
 * its capability told, then its provisions go, then its own capability watches stop.
 */
void weivel_capabilities_strip(weivel_core *core, weivel_plugin *plugin);

/** Stops the capability watch `handle` names; false when it names none. */
bool weivel_capabilities_end_watch(weivel_core *core, weivel_handle handle);

#endif
