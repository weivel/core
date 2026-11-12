#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "registry.h"

static weivel_reach_record *append_record(weivel_registry *registry, weivel_plugin *plugin, uint32_t kind,
                                          const char *id) {
    if (plugin->reported_count == plugin->reported_capacity) {
        if (plugin->reported_capacity > UINT32_MAX / 2) {
            return NULL;
        }
        uint32_t capacity = plugin->reported_capacity == 0 ? 4 : plugin->reported_capacity * 2;
        weivel_reach_record *grown = realloc(plugin->reported, (size_t)capacity * sizeof *grown);
        if (grown == NULL) {
            return NULL;
        }
        plugin->reported = grown;
        plugin->reported_capacity = capacity;
    }
    size_t length = strlen(id) + 1;
    char *copy = malloc(length);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, id, length);
    weivel_reach_record *record = &plugin->reported[plugin->reported_count++];
    record->kind = kind;
    record->plugin = plugin->handle;
    record->id = copy;
    record->sequence = registry->next_report++;
    return record;
}

/**
 * Reports a refusal the first time this plugin meets it for this kind and id; every later time it is answered
 * alone.
 */
static void report_kind_once(weivel_core *core, weivel_plugin *plugin, uint32_t kind, const char *id,
                             const weivel_reach *reach) {
    weivel_table *reported =
        kind == WEIVEL_REACH_CAPABILITY ? plugin->reported_capabilities : plugin->reported_services;
    if (weivel_table_find_text(reported, id) != NULL) {
        return;
    }
    weivel_reach_record *record = append_record(core->registry, plugin, kind, id);
    if (record == NULL) {
        return;
    }
    if (weivel_table_add_text(reported, record->id, record->id) != WEIVEL_STATUS_OK) {
        free(record->id);
        plugin->reported_count--;
        return;
    }
    weivel_events_push_reach(core->events, kind, plugin->handle, id, reach->code, reach->detail, reach->fix);
}

static void report_once(weivel_core *core, weivel_plugin *plugin, const char *id, const weivel_reach *reach) {
    report_kind_once(core, plugin, WEIVEL_REACH_SERVICE, id, reach);
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
    reach->late = false;
    reach->provider = provider;
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
    reach->wanted = declaration->wanted;
    const weivel_slot *slot = weivel_registry_find(core->registry, id);
    const weivel_registration *owner = slot == NULL ? NULL : slot->registration;
    if (owner != NULL && !weivel_registry_accepts(reach->provider, owner)) {
        weivel_reach_refuse_provider(core, plugin, id, reach->provider, weivel_registry_owner_name(owner), reach);
        return;
    }
    reach->late = plugin->state != WEIVEL_PLUGIN_OPEN;
}

static void log_late(weivel_core *core, const weivel_plugin *plugin, const char *what, const char *noun,
                     const char *id) {
    char text[512];
    snprintf(text, sizeof text, "ignored a late %s of %s \"%s\" from %s, which is no longer running", what, noun,
             id, plugin->identity);
    weivel_core_log(core, WEIVEL_LOG_WARNING, text);
}

void weivel_reach_log_late(weivel_core *core, const weivel_plugin *plugin, const char *what, const char *id) {
    log_late(core, plugin, what, "service", id);
}

void weivel_reach_log_late_capability(weivel_core *core, const weivel_plugin *plugin, const char *what,
                                      const char *id) {
    log_late(core, plugin, what, "capability", id);
}

bool weivel_reach_capability(weivel_core *core, weivel_plugin *plugin, const char *id) {
    if (weivel_table_find_text(plugin->capabilities_reachable, id) != NULL) {
        return true;
    }
    weivel_reach reach;
    reach.code = WEIVEL_REFUSAL_UNDECLARED;
    reach.late = false;
    reach.provider = NULL;
    reach.wanted = NULL;
    snprintf(reach.detail, sizeof reach.detail, "reached for capability \"%s\", which it does not declare", id);
    snprintf(reach.fix, sizeof reach.fix, "add \"%s\" to the capabilities it consumes in its manifest", id);
    report_kind_once(core, plugin, WEIVEL_REACH_CAPABILITY, id, &reach);
    return false;
}

void weivel_reach_stopped(const char *id, weivel_reach *reach) {
    memset(reach, 0, sizeof *reach);
    reach->code = WEIVEL_REFUSAL_STOPPED;
    snprintf(reach->detail, sizeof reach->detail, "stopped while waiting for service \"%s\"", id);
    snprintf(reach->fix, sizeof reach->fix,
             "provide \"%s\" before this plugin is stopped, or get it and carry on without it", id);
}

uint32_t weivel_reach_reported_count(const weivel_plugin *plugin) {
    return plugin->reported_count;
}

const weivel_reach_record *weivel_reach_reported_at(const weivel_plugin *plugin, uint32_t index) {
    return index < plugin->reported_count ? &plugin->reported[index] : NULL;
}
