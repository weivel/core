#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "memory.h"
#include "registry.h"
#include "text.h"

static weivel_reach_record *append_record(weivel_registry *registry, weivel_plugin *plugin, uint32_t kind,
                                          uint32_t code, const char *id) {
    if (plugin->reported_count == plugin->reported_capacity) {
        if (plugin->reported_capacity > UINT32_MAX / 2) {
            return NULL;
        }
        uint32_t capacity = plugin->reported_capacity == 0 ? 4 : plugin->reported_capacity * 2;
        weivel_reach_record *grown = weivel_memory_reallocate(plugin->reported, (size_t)capacity * sizeof *grown);
        if (grown == NULL) {
            return NULL;
        }
        plugin->reported = grown;
        plugin->reported_capacity = capacity;
    }
    size_t length = strlen(id) + 1;
    char *copy = weivel_memory_allocate(length);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, id, length);
    weivel_reach_record *record = &plugin->reported[plugin->reported_count++];
    record->kind = kind;
    record->code = code;
    record->plugin = plugin->handle;
    record->id = copy;
    record->sequence = registry->next_report++;
    return record;
}

static weivel_table *reported_of(weivel_plugin *plugin, uint32_t kind) {
    return kind == WEIVEL_REACH_CAPABILITY ? plugin->reported_capabilities : plugin->reported_services;
}

/** Takes back the plugin's newest record, which the call now running made and is undoing. */
static void pop_record(weivel_core *core, weivel_plugin *plugin) {
    weivel_reach_record *record = &plugin->reported[--plugin->reported_count];
    weivel_table_remove_text(reported_of(plugin, record->kind), record->id);
    free(record->id);
    core->registry->next_report--;
}

/**
 * Reports a refusal the first time this plugin meets it for this kind and id; every later time it is answered
 * alone. All or nothing: when memory runs out for the record, its texts or its event, nothing is recorded, and the
 * refusal is reported the next time it is met.
 */
static weivel_status report_kind_once(weivel_core *core, weivel_plugin *plugin, uint32_t kind, const char *id,
                                      const weivel_reach *reach) {
    weivel_table *reported = reported_of(plugin, kind);
    if (weivel_table_find_text(reported, id) != NULL) {
        return WEIVEL_STATUS_OK;
    }
    if (reach->detail == NULL || reach->fix == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_reach_record *record = append_record(core->registry, plugin, kind, reach->code, id);
    if (record == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    if (weivel_table_add_text(reported, record->id, record->id) != WEIVEL_STATUS_OK) {
        free(record->id);
        plugin->reported_count--;
        core->registry->next_report--;
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_status status =
        weivel_events_push_reach(core->events, kind, plugin->handle, id, reach->code, reach->detail, reach->fix);
    if (status != WEIVEL_STATUS_OK) {
        pop_record(core, plugin);
    }
    return status;
}

void weivel_reach_unreport_since(weivel_core *core, weivel_plugin *plugin, uint64_t since) {
    if (plugin->reported_count == 0 || plugin->reported[plugin->reported_count - 1].sequence < since) {
        return;
    }
    pop_record(core, plugin);
    weivel_events_drop_newest(core->events);
}

static weivel_status report_once(weivel_core *core, weivel_plugin *plugin, const char *id,
                                 const weivel_reach *reach) {
    return report_kind_once(core, plugin, WEIVEL_REACH_SERVICE, id, reach);
}

static weivel_status refuse_undeclared(weivel_core *core, weivel_plugin *plugin, const char *id,
                                       weivel_reach *reach) {
    reach->code = WEIVEL_REFUSAL_UNDECLARED;
    reach->detail = weivel_text_format("reached for service \"%s\", which it does not declare", id);
    reach->fix = weivel_text_format("add \"%s\" to the services it consumes in its manifest", id);
    return report_once(core, plugin, id, reach);
}

weivel_status weivel_reach_refuse_provider(weivel_core *core, weivel_plugin *plugin, const char *id,
                                           const char *provider, const char *owner, weivel_reach *reach) {
    reach->code = WEIVEL_REFUSAL_WRONG_PROVIDER;
    reach->late = false;
    reach->provider = provider;
    reach->wanted = NULL;
    reach->detail =
        weivel_text_format("reached for service \"%s\", which it binds to %s but %s provides", id, provider, owner);
    reach->fix = weivel_text_format(
        "install %s, or remove the provider from \"%s\" in the services it consumes in its manifest", provider, id);
    return report_once(core, plugin, id, reach);
}

weivel_status weivel_reach_check(weivel_core *core, weivel_plugin *plugin, const char *id, weivel_reach *reach) {
    memset(reach, 0, sizeof *reach);
    if (plugin->is_host) {
        return WEIVEL_STATUS_OK;
    }
    const weivel_declaration *declaration = weivel_table_find_text(plugin->declared, id);
    if (declaration == NULL) {
        return refuse_undeclared(core, plugin, id, reach);
    }
    reach->provider = declaration->provider;
    reach->wanted = declaration->wanted;
    const weivel_slot *slot = weivel_registry_find(core->registry, id);
    const weivel_registration *owner = slot == NULL ? NULL : slot->registration;
    if (owner != NULL && !weivel_registry_accepts(reach->provider, owner)) {
        return weivel_reach_refuse_provider(core, plugin, id, reach->provider, weivel_registry_owner_name(owner),
                                            reach);
    }
    reach->late = plugin->state != WEIVEL_PLUGIN_OPEN;
    return WEIVEL_STATUS_OK;
}

static void log_late(weivel_core *core, const weivel_plugin *plugin, const char *what, const char *noun,
                     const char *id) {
    char *text = weivel_text_format("ignored a late %s of %s \"%s\" from %s, which is no longer running", what,
                                    noun, id, plugin->identity);
    if (text == NULL) {
        weivel_core_log(core, WEIVEL_LOG_ERROR, "memory ran out describing a late call, which was ignored");
        return;
    }
    weivel_core_log(core, WEIVEL_LOG_WARNING, text);
    free(text);
}

void weivel_reach_log_late(weivel_core *core, const weivel_plugin *plugin, const char *what, const char *id) {
    log_late(core, plugin, what, "service", id);
}

void weivel_reach_log_late_capability(weivel_core *core, const weivel_plugin *plugin, const char *what,
                                      const char *id) {
    log_late(core, plugin, what, "capability", id);
}

weivel_status weivel_reach_capability(weivel_core *core, weivel_plugin *plugin, const char *id) {
    if (plugin->is_host || weivel_table_find_text(plugin->capabilities_reachable, id) != NULL) {
        return WEIVEL_STATUS_OK;
    }
    weivel_reach reach;
    memset(&reach, 0, sizeof reach);
    reach.code = WEIVEL_REFUSAL_UNDECLARED;
    reach.detail = weivel_text_format("reached for capability \"%s\", which it does not declare", id);
    reach.fix = weivel_text_format("add \"%s\" to the capabilities it consumes in its manifest", id);
    weivel_status status = report_kind_once(core, plugin, WEIVEL_REACH_CAPABILITY, id, &reach);
    weivel_reach_dispose(&reach);
    return status == WEIVEL_STATUS_OK ? WEIVEL_STATUS_REFUSED : status;
}

void weivel_reach_stopped(const char *id, weivel_reach *reach) {
    memset(reach, 0, sizeof *reach);
    reach->code = WEIVEL_REFUSAL_STOPPED;
    reach->detail = weivel_text_format("stopped while waiting for service \"%s\"", id);
    reach->fix =
        weivel_text_format("provide \"%s\" before this plugin is stopped, or get it and carry on without it", id);
}

void weivel_reach_dispose(weivel_reach *reach) {
    free(reach->detail);
    free(reach->fix);
    reach->detail = NULL;
    reach->fix = NULL;
}

uint32_t weivel_reach_reported_count(const weivel_plugin *plugin) {
    return plugin->reported_count;
}

const weivel_reach_record *weivel_reach_reported_at(const weivel_plugin *plugin, uint32_t index) {
    return index < plugin->reported_count ? &plugin->reported[index] : NULL;
}
