#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "registry.h"

static weivel_reach_record *append_record(weivel_registry *registry, const weivel_plugin *plugin, const char *id) {
    if (registry->reported_count == registry->reported_capacity) {
        if (registry->reported_capacity > UINT32_MAX / 2) {
            return NULL;
        }
        uint32_t capacity = registry->reported_capacity == 0 ? 16 : registry->reported_capacity * 2;
        weivel_reach_record *grown = realloc(registry->reported, (size_t)capacity * sizeof *grown);
        if (grown == NULL) {
            return NULL;
        }
        registry->reported = grown;
        registry->reported_capacity = capacity;
    }
    size_t length = strlen(id) + 1;
    char *copy = malloc(length);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, id, length);
    weivel_reach_record *record = &registry->reported[registry->reported_count++];
    record->kind = WEIVEL_REACH_SERVICE;
    record->plugin = plugin->handle;
    record->id = copy;
    return record;
}

/** Reports a refusal the first time this plugin meets it for this id; every later time it is answered alone. */
static void report_once(weivel_core *core, weivel_plugin *plugin, const char *id, const weivel_reach *reach) {
    if (weivel_table_find_text(plugin->reported_services, id) != NULL) {
        return;
    }
    weivel_reach_record *record = append_record(core->registry, plugin, id);
    if (record == NULL) {
        return;
    }
    if (weivel_table_add_text(plugin->reported_services, record->id, record->id) != WEIVEL_STATUS_OK) {
        free(record->id);
        core->registry->reported_count--;
        return;
    }
    weivel_events_push_reach(core->events, WEIVEL_REACH_SERVICE, plugin->handle, id, reach->code, reach->detail,
                             reach->fix);
}

static void refuse_undeclared(weivel_core *core, weivel_plugin *plugin, const char *id, weivel_reach *reach) {
    reach->code = WEIVEL_REFUSAL_UNDECLARED;
    snprintf(reach->detail, sizeof reach->detail, "reached for service \"%s\", which it does not declare", id);
    snprintf(reach->fix, sizeof reach->fix, "add \"%s\" to the services it consumes in its manifest", id);
    report_once(core, plugin, id, reach);
}

void weivel_reach_refuse_provider(weivel_core *core, weivel_plugin *plugin, const char *id, const char *provider,
                                  const char *owner, weivel_reach *reach) {
    reach->code = WEIVEL_REFUSAL_WRONG_PROVIDER;
    snprintf(reach->detail, sizeof reach->detail, "reached for service \"%s\", which it binds to %s but %s provides",
             id, provider, owner);
    snprintf(reach->fix, sizeof reach->fix,
             "install %s, or remove the provider from \"%s\" in the services it consumes in its manifest", provider,
             id);
    report_once(core, plugin, id, reach);
}

void weivel_reach_check(weivel_core *core, weivel_plugin *plugin, const char *id, weivel_reach *reach) {
    memset(reach, 0, sizeof *reach);
    const weivel_declaration *declaration = weivel_table_find_text(plugin->declared, id);
    if (declaration == NULL) {
        refuse_undeclared(core, plugin, id, reach);
        return;
    }
    reach->provider = declaration->provider;
    const weivel_slot *slot = weivel_registry_find(core->registry, id);
    const weivel_registration *owner = slot == NULL ? NULL : slot->registration;
    if (owner != NULL && !weivel_registry_accepts(reach->provider, owner)) {
        weivel_reach_refuse_provider(core, plugin, id, reach->provider, weivel_registry_owner_name(owner), reach);
        return;
    }
    reach->late = plugin->state != WEIVEL_PLUGIN_OPEN;
}

void weivel_reach_log_late(weivel_core *core, const weivel_plugin *plugin, const char *what, const char *id) {
    char text[512];
    const char *name = plugin->identity != NULL ? plugin->identity : plugin->manifest.value.import_name;
    snprintf(text, sizeof text, "ignored a late %s of service \"%s\" from %s, which is no longer running", what, id,
             name);
    weivel_core_log(core, WEIVEL_LOG_WARNING, text);
}

void weivel_reach_stopped(const char *id, weivel_reach *reach) {
    memset(reach, 0, sizeof *reach);
    reach->code = WEIVEL_REFUSAL_STOPPED;
    snprintf(reach->detail, sizeof reach->detail, "stopped while waiting for service \"%s\"", id);
    snprintf(reach->fix, sizeof reach->fix,
             "provide \"%s\" before this plugin is stopped, or get it and carry on without it", id);
}

uint32_t weivel_reach_reported_count(const weivel_core *core) {
    return core->registry->reported_count;
}

const weivel_reach_record *weivel_reach_reported_at(const weivel_core *core, uint32_t index) {
    return index < core->registry->reported_count ? &core->registry->reported[index] : NULL;
}
