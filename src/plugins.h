#ifndef WEIVEL_PLUGINS_H
#define WEIVEL_PLUGINS_H

#include <weivel/core.h>

#include "manifest.h"

/** A registered plugin: the manifest it was registered with, owned by the core, and the handle that names it. */
typedef struct weivel_plugin {
    weivel_owned_manifest manifest;
    weivel_handle handle;
} weivel_plugin;

/** Every plugin record one core owns; the handle table only borrows them. */
typedef struct weivel_plugins weivel_plugins;

weivel_plugins *weivel_plugins_create(void);

/** Frees the collection and every plugin in it, manifests and all. */
void weivel_plugins_destroy(weivel_plugins *plugins);

/** Takes ownership of `plugin` on WEIVEL_STATUS_OK; on WEIVEL_STATUS_OUT_OF_MEMORY the caller still owns it. */
weivel_status weivel_plugins_add(weivel_plugins *plugins, weivel_plugin *plugin);

#endif
