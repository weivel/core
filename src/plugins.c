#include "plugins.h"

#include <stdlib.h>

#include "core_internal.h"
#include "sized.h"

struct weivel_plugins {
    weivel_plugin **items;
    uint32_t count;
    uint32_t capacity;
};

weivel_plugins *weivel_plugins_create(void) {
    return calloc(1, sizeof(weivel_plugins));
}

static void free_plugin(weivel_plugin *plugin) {
    weivel_manifest_free(&plugin->manifest);
    free(plugin);
}

void weivel_plugins_destroy(weivel_plugins *plugins) {
    if (plugins == NULL) {
        return;
    }
    for (uint32_t index = 0; index < plugins->count; index++) {
        free_plugin(plugins->items[index]);
    }
    free(plugins->items);
    free(plugins);
}

static bool grow(weivel_plugins *plugins) {
    if (plugins->capacity > UINT32_MAX / 2) {
        return false;
    }
    uint32_t capacity = plugins->capacity == 0 ? 16 : plugins->capacity * 2;
    weivel_plugin **items = realloc(plugins->items, (size_t)capacity * sizeof *items);
    if (items == NULL) {
        return false;
    }
    plugins->items = items;
    plugins->capacity = capacity;
    return true;
}

weivel_status weivel_plugins_add(weivel_plugins *plugins, weivel_plugin *plugin) {
    if (plugins->count == plugins->capacity && !grow(plugins)) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    plugins->items[plugins->count++] = plugin;
    return WEIVEL_STATUS_OK;
}

static weivel_status refuse_manifest(weivel_core *core, const weivel_manifest_problem *problem) {
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_MALFORMED_MANIFEST, problem->detail, problem->fix);
    return WEIVEL_STATUS_REFUSED;
}

static weivel_status keep_plugin(weivel_core *core, weivel_plugin *plugin) {
    weivel_status status = weivel_handles_allocate(core->handles, WEIVEL_RECORD_PLUGIN, plugin, &plugin->handle);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    status = weivel_plugins_add(core->plugins, plugin);
    if (status != WEIVEL_STATUS_OK) {
        weivel_handles_release(core->handles, plugin->handle, WEIVEL_RECORD_PLUGIN);
    }
    return status;
}

static weivel_status register_manifest(weivel_core *core, const weivel_manifest *manifest, weivel_handle *handle) {
    weivel_plugin *plugin = calloc(1, sizeof *plugin);
    if (plugin == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_manifest_problem problem;
    weivel_status status = weivel_manifest_copy(manifest, &plugin->manifest, &problem);
    if (status == WEIVEL_STATUS_OK) {
        status = keep_plugin(core, plugin);
    } else if (status == WEIVEL_STATUS_REFUSED) {
        refuse_manifest(core, &problem);
    }
    if (status != WEIVEL_STATUS_OK) {
        free_plugin(plugin);
        return status;
    }
    *handle = plugin->handle;
    return WEIVEL_STATUS_OK;
}

weivel_status weivel_register_manifest(weivel_core *core, const weivel_manifest *manifest, weivel_handle *plugin,
                                       uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (plugin == NULL) {
        status = weivel_core_refuse_malformed(core, "weivel_register_manifest was given no place to write the handle",
                                              "pass a pointer to a weivel_handle");
    } else if (manifest == NULL || weivel_sized_size(manifest) < WEIVEL_MANIFEST_FIRST_SIZE) {
        *plugin = 0;
        status = weivel_core_refuse_malformed(
            core, "weivel_register_manifest was given no manifest, or one whose size is too small",
            "pass a weivel_manifest initialised with WEIVEL_INIT(weivel_manifest)");
    } else {
        *plugin = 0;
        status = register_manifest(core, manifest, plugin);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}
