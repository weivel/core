#include "plugins.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "sized.h"
#include "text.h"

struct weivel_plugins {
    weivel_plugin **items;
    uint32_t count;
    uint32_t capacity;
    /** From the import name of each plugin that is not released to the plugin. */
    weivel_table *names;
    /** From the identity of each plugin that is not released to the plugin. */
    weivel_table *identities;
    weivel_link order;
};

weivel_plugins *weivel_plugins_create(void) {
    weivel_plugins *plugins = calloc(1, sizeof *plugins);
    if (plugins == NULL) {
        return NULL;
    }
    weivel_list_init(&plugins->order);
    plugins->names = weivel_table_create();
    plugins->identities = weivel_table_create();
    if (plugins->names == NULL || plugins->identities == NULL) {
        weivel_plugins_destroy(plugins);
        return NULL;
    }
    return plugins;
}

static void free_plugin(weivel_plugin *plugin) {
    weivel_table_destroy(plugin->declared);
    weivel_table_destroy(plugin->reported_services);
    weivel_table_destroy(plugin->capabilities_reachable);
    weivel_table_destroy(plugin->capabilities_declared);
    weivel_table_destroy(plugin->provided);
    weivel_table_destroy(plugin->reported_capabilities);
    for (uint32_t index = 0; index < plugin->reported_count; index++) {
        free(plugin->reported[index].id);
    }
    free(plugin->reported);
    free(plugin->declarations);
    weivel_ledger_free_plugin(plugin);
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
    weivel_table_destroy(plugins->names);
    weivel_table_destroy(plugins->identities);
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

static weivel_status add_plugin(weivel_plugins *plugins, weivel_plugin *plugin) {
    if (plugins->count == plugins->capacity && !grow(plugins)) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_status status = weivel_table_add_text(plugins->names, plugin->manifest.value.import_name, plugin);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    status = weivel_table_add_text(plugins->identities, plugin->identity, plugin);
    if (status != WEIVEL_STATUS_OK) {
        weivel_table_remove_text(plugins->names, plugin->manifest.value.import_name);
        return status;
    }
    plugin->index = plugins->count;
    plugins->items[plugins->count++] = plugin;
    weivel_list_append(&plugins->order, &plugin->in_order);
    return WEIVEL_STATUS_OK;
}

static void remove_plugin(weivel_plugins *plugins, weivel_plugin *plugin) {
    weivel_plugin *last = plugins->items[--plugins->count];
    plugins->items[plugin->index] = last;
    last->index = plugin->index;
    weivel_list_remove(&plugin->in_order);
}

uint32_t weivel_plugins_count(const weivel_plugins *plugins) {
    return plugins->count;
}

const weivel_link *weivel_plugins_in_order(const weivel_plugins *plugins) {
    return &plugins->order;
}

bool weivel_plugins_named(const weivel_plugins *plugins, const char *import_name) {
    return weivel_table_find_text(plugins->names, import_name) != NULL;
}

weivel_plugin *weivel_plugins_with_identity(const weivel_plugins *plugins, const char *identity) {
    return weivel_table_find_text(plugins->identities, identity);
}

static weivel_plugin *new_plugin(void) {
    weivel_plugin *plugin = calloc(1, sizeof *plugin);
    if (plugin == NULL) {
        return NULL;
    }
    weivel_list_init(&plugin->registrations);
    weivel_list_init(&plugin->wants);
    weivel_list_init(&plugin->watches);
    weivel_list_init(&plugin->holdings);
    weivel_list_init(&plugin->provisions);
    weivel_list_init(&plugin->capability_watches);
    weivel_list_init(&plugin->capability_holdings);
    weivel_list_init(&plugin->in_order);
    plugin->declared = weivel_table_create();
    plugin->reported_services = weivel_table_create();
    plugin->capabilities_reachable = weivel_table_create();
    plugin->capabilities_declared = weivel_table_create();
    plugin->provided = weivel_table_create();
    plugin->reported_capabilities = weivel_table_create();
    if (plugin->declared == NULL || plugin->reported_services == NULL || plugin->capabilities_reachable == NULL ||
        plugin->capabilities_declared == NULL || plugin->provided == NULL || plugin->reported_capabilities == NULL) {
        free_plugin(plugin);
        return NULL;
    }
    return plugin;
}

static weivel_status declare(weivel_plugin *plugin, const char *id, const weivel_service_want *wanted,
                             const weivel_service_offer *offered) {
    weivel_declaration *declaration = weivel_table_find_text(plugin->declared, id);
    if (declaration != NULL) {
        declaration->offered = offered;
        return WEIVEL_STATUS_OK;
    }
    declaration = &plugin->declarations[plugin->declaration_count++];
    declaration->id = id;
    declaration->provider = wanted == NULL ? NULL : wanted->provider;
    declaration->consumed = wanted != NULL;
    declaration->wanted = wanted;
    declaration->offered = offered;
    return weivel_table_add_text(plugin->declared, id, declaration);
}

static weivel_status declare_services(weivel_plugin *plugin) {
    const weivel_service_declarations *services = &plugin->manifest.value.services;
    size_t total = (size_t)services->consumes.count + services->provides.count;
    plugin->declarations = calloc(total == 0 ? 1 : total, sizeof *plugin->declarations);
    if (plugin->declarations == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_status status = WEIVEL_STATUS_OK;
    for (uint32_t index = 0; status == WEIVEL_STATUS_OK && index < services->consumes.count; index++) {
        const weivel_service_want *want = &services->consumes.items[index];
        status = declare(plugin, want->id, want, NULL);
    }
    for (uint32_t index = 0; status == WEIVEL_STATUS_OK && index < services->provides.count; index++) {
        const weivel_service_offer *offer = &services->provides.items[index];
        status = declare(plugin, offer->id, NULL, offer);
    }
    return status;
}

static weivel_status index_ids(weivel_table *table, const weivel_strings *ids) {
    weivel_status status = WEIVEL_STATUS_OK;
    for (uint32_t index = 0; status == WEIVEL_STATUS_OK && index < ids->count; index++) {
        status = weivel_table_add_text(table, ids->items[index], (void *)ids->items[index]);
    }
    return status;
}

static weivel_status declare_capabilities(weivel_plugin *plugin) {
    const weivel_capability_declarations *capabilities = &plugin->manifest.value.capabilities;
    weivel_status status = index_ids(plugin->capabilities_declared, &capabilities->provides);
    if (status == WEIVEL_STATUS_OK) {
        status = index_ids(plugin->capabilities_reachable, &capabilities->consumes);
    }
    if (status == WEIVEL_STATUS_OK) {
        status = index_ids(plugin->capabilities_reachable, &capabilities->provides);
    }
    return status;
}

static weivel_status refuse_taken_name(weivel_core *core, const char *import_name) {
    char detail[384];
    snprintf(detail, sizeof detail, "a plugin named \"%s\" is already registered", import_name);
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_NAME_TAKEN, detail,
                               "give this plugin its own import name, or release the plugin registered under it");
    return WEIVEL_STATUS_REFUSED;
}

static weivel_status refuse_identity(weivel_core *core, const char *identity) {
    char detail[384];
    snprintf(detail, sizeof detail, "a plugin with the identity \"%s\" is already registered", identity);
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_IDENTITY_TAKEN, detail,
                               "give each plugin its own identity, or release the plugin that holds it");
    return WEIVEL_STATUS_REFUSED;
}

static weivel_status refuse_manifest(weivel_core *core, const weivel_manifest_problem *problem) {
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_MALFORMED_MANIFEST, problem->detail, problem->fix);
    return WEIVEL_STATUS_REFUSED;
}

static weivel_status keep_plugin(weivel_core *core, weivel_plugin *plugin) {
    weivel_status status = declare_services(plugin);
    if (status == WEIVEL_STATUS_OK) {
        status = declare_capabilities(plugin);
    }
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    status = weivel_handles_allocate(core->handles, WEIVEL_RECORD_PLUGIN, plugin, &plugin->handle);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    status = add_plugin(core->plugins, plugin);
    if (status != WEIVEL_STATUS_OK) {
        weivel_handles_release(core->handles, plugin->handle, WEIVEL_RECORD_PLUGIN);
    }
    return status;
}

static weivel_status register_manifest(weivel_core *core, const weivel_manifest *manifest, weivel_handle *handle) {
    weivel_plugin *plugin = new_plugin();
    if (plugin == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_manifest_problem problem;
    weivel_status status = weivel_manifest_copy(manifest, &plugin->manifest, &problem);
    plugin->identity = plugin->manifest.value.identity;
    if (status == WEIVEL_STATUS_OK && weivel_plugins_named(core->plugins, plugin->manifest.value.import_name)) {
        status = refuse_taken_name(core, plugin->manifest.value.import_name);
    } else if (status == WEIVEL_STATUS_OK && weivel_plugins_with_identity(core->plugins, plugin->identity) != NULL) {
        status = refuse_identity(core, plugin->identity);
    } else if (status == WEIVEL_STATUS_OK) {
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

weivel_status weivel_core_plugin(weivel_core *core, weivel_handle handle, weivel_plugin **plugin) {
    void *record;
    if (weivel_handles_resolve(core->handles, handle, WEIVEL_RECORD_PLUGIN, &record) == WEIVEL_STATUS_OK) {
        *plugin = record;
        return WEIVEL_STATUS_OK;
    }
    char detail[96];
    snprintf(detail, sizeof detail, "the handle %" PRIu64 " names no plugin on this core", handle);
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_STALE_HANDLE, detail,
                               "pass only handles weivel_register_manifest answered on this core");
    *plugin = NULL;
    return WEIVEL_STATUS_REFUSED;
}

static weivel_status refuse_state(weivel_core *core, const weivel_plugin *plugin, const char *what, const char *fix) {
    char detail[384];
    snprintf(detail, sizeof detail, "the plugin \"%s\" %s", plugin->manifest.value.import_name, what);
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_PLUGIN_STATE, detail, fix);
    return WEIVEL_STATUS_REFUSED;
}

static weivel_status refuse_released(weivel_core *core, const weivel_plugin *plugin) {
    return refuse_state(core, plugin, "was released",
                        "register its manifest again, and use the handle that answers");
}

weivel_status weivel_core_opened_plugin(weivel_core *core, weivel_handle handle, weivel_plugin **plugin) {
    weivel_status status = weivel_core_plugin(core, handle, plugin);
    if (status == WEIVEL_STATUS_OK && (*plugin)->state == WEIVEL_PLUGIN_REGISTERED) {
        status = refuse_state(core, *plugin, "has not been opened", "open it with weivel_open_plugin first");
    }
    return status;
}

static void forget_identity(weivel_core *core, weivel_plugin *plugin) {
    if (weivel_plugins_with_identity(core->plugins, plugin->identity) == plugin) {
        weivel_table_remove_text(core->plugins->identities, plugin->identity);
    }
}

static weivel_status open_plugin(weivel_core *core, weivel_plugin *plugin, const weivel_plugin_options *options) {
    if (plugin->state == WEIVEL_PLUGIN_OPEN) {
        return refuse_state(core, plugin, "is already open", "release or quarantine it before opening it again");
    }
    if (plugin->state == WEIVEL_PLUGIN_RELEASED) {
        return refuse_released(core, plugin);
    }
    plugin->payload = options->payload;
    plugin->state = WEIVEL_PLUGIN_OPEN;
    weivel_ledger_begin_activation(plugin);
    return WEIVEL_STATUS_OK;
}

static bool read_plugin_options(const weivel_plugin_options *options, weivel_plugin_options *resolved) {
    memset(resolved, 0, sizeof *resolved);
    if (options == NULL) {
        return true;
    }
    if (weivel_sized_size(options) < WEIVEL_PLUGIN_OPTIONS_FIRST_SIZE) {
        return false;
    }
    weivel_sized_read(resolved, sizeof *resolved, options);
    return true;
}

weivel_status weivel_open_plugin(weivel_core *core, weivel_handle plugin, const weivel_plugin_options *options,
                                 uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_plugin_options resolved;
    weivel_status status;
    weivel_plugin *record = NULL;
    if (!read_plugin_options(options, &resolved)) {
        status = weivel_core_refuse_malformed(core, "weivel_open_plugin was given options whose size is too small",
                                              "pass weivel_plugin_options initialised with WEIVEL_INIT");
    } else if ((status = weivel_core_plugin(core, plugin, &record)) == WEIVEL_STATUS_OK) {
        status = open_plugin(core, record, &resolved);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

weivel_status weivel_plugin_payload(weivel_core *core, weivel_handle plugin, uint64_t *payload,
                                    uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    weivel_plugin *record = NULL;
    if (payload == NULL) {
        status = weivel_core_refuse_malformed(core, "weivel_plugin_payload was given no place to write the payload",
                                              "pass a pointer to a uint64_t");
    } else if ((status = weivel_core_plugin(core, plugin, &record)) == WEIVEL_STATUS_OK) {
        *payload = record->payload;
    } else {
        *payload = 0;
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

/**
 * @implNote Capabilities go first, so a plugin watching what it provides hears its own implementations withdrawn
 * before its watches stop.
 */
static void strip_plugin(weivel_core *core, weivel_plugin *plugin) {
    weivel_capabilities_strip(core, plugin);
    weivel_registry_strip(core, plugin);
}

static void release_plugin(weivel_core *core, weivel_plugin *plugin) {
    if (plugin->state == WEIVEL_PLUGIN_RELEASED) {
        return;
    }
    strip_plugin(core, plugin);
    forget_identity(core, plugin);
    weivel_table_remove_text(core->plugins->names, plugin->manifest.value.import_name);
    plugin->state = WEIVEL_PLUGIN_RELEASED;
}

static void log_quarantined(weivel_core *core, const weivel_plugin *plugin) {
    const weivel_kept_refusal *why = weivel_kept_refusal_held(&plugin->quarantine_reason)
                                         ? &plugin->quarantine_reason
                                         : &plugin->activation_refusal;
    char *text = weivel_text_format("quarantined the plugin \"%s\": %s", plugin->manifest.value.import_name,
                                    weivel_kept_refusal_held(why) ? why->detail : "no reason was given");
    if (text == NULL) {
        weivel_core_log(core, WEIVEL_LOG_ERROR, "memory ran out describing a quarantine");
        return;
    }
    weivel_core_log(core, WEIVEL_LOG_INFO, text);
    free(text);
}

static weivel_status quarantine_plugin(weivel_core *core, weivel_plugin *plugin,
                                       const weivel_quarantine_reason *reason) {
    if (plugin->state == WEIVEL_PLUGIN_RELEASED) {
        return refuse_released(core, plugin);
    }
    weivel_kept_refusal given = {WEIVEL_REFUSAL_NONE, NULL, NULL};
    if (reason != NULL && !weivel_kept_refusal_copy(&given, reason->code, reason->detail, reason->fix)) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    strip_plugin(core, plugin);
    plugin->state = WEIVEL_PLUGIN_QUARANTINED;
    if (reason != NULL) {
        weivel_kept_refusal_clear(&plugin->quarantine_reason);
        plugin->quarantine_reason = given;
    }
    log_quarantined(core, plugin);
    return WEIVEL_STATUS_OK;
}

weivel_status weivel_release_plugin(weivel_core *core, weivel_handle plugin, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_plugin *record = NULL;
    weivel_status status = weivel_core_plugin(core, plugin, &record);
    if (status == WEIVEL_STATUS_OK) {
        release_plugin(core, record);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

/** Reads a caller's reason into `resolved`; false when it is too small or misses a text. */
static bool read_reason(const weivel_quarantine_reason *reason, weivel_quarantine_reason *resolved) {
    if (weivel_sized_size(reason) < WEIVEL_QUARANTINE_REASON_FIRST_SIZE) {
        return false;
    }
    weivel_sized_read(resolved, sizeof *resolved, reason);
    return resolved->detail != NULL && resolved->fix != NULL;
}

weivel_status weivel_quarantine_plugin(weivel_core *core, weivel_handle plugin,
                                       const weivel_quarantine_reason *reason, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_quarantine_reason resolved;
    weivel_plugin *record = NULL;
    weivel_status status;
    if (reason != NULL && !read_reason(reason, &resolved)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_quarantine_plugin was given a reason whose size is too small, or with no detail or fix",
            "pass NULL, or a weivel_quarantine_reason initialised with WEIVEL_INIT with its detail and fix set");
    } else if ((status = weivel_core_plugin(core, plugin, &record)) == WEIVEL_STATUS_OK) {
        status = quarantine_plugin(core, record, reason == NULL ? NULL : &resolved);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

static weivel_status forget_plugin(weivel_core *core, weivel_plugin *plugin) {
    if (plugin->state != WEIVEL_PLUGIN_RELEASED) {
        return refuse_state(core, plugin, "is not released", "release it before forgetting it");
    }
    strip_plugin(core, plugin);
    weivel_handles_release(core->handles, plugin->handle, WEIVEL_RECORD_PLUGIN);
    remove_plugin(core->plugins, plugin);
    free_plugin(plugin);
    return WEIVEL_STATUS_OK;
}

weivel_status weivel_forget_plugin(weivel_core *core, weivel_handle plugin, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_plugin *record = NULL;
    weivel_status status = weivel_core_plugin(core, plugin, &record);
    if (status == WEIVEL_STATUS_OK) {
        status = forget_plugin(core, record);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}
