#include "registry.h"
#include "memory.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "sized.h"
#include "text.h"

weivel_registry *weivel_registry_create(void) {
    weivel_registry *registry = weivel_memory_allocate_zeroed(1, sizeof *registry);
    if (registry == NULL) {
        return NULL;
    }
    weivel_list_init(&registry->all_slots);
    registry->slots = weivel_table_create();
    registry->holdings = weivel_table_create();
    if (registry->slots == NULL || registry->holdings == NULL) {
        weivel_registry_destroy(registry);
        return NULL;
    }
    return registry;
}

static void free_registration(weivel_registration *registration) {
    while (!weivel_list_empty(&registration->holdings)) {
        weivel_link *link = registration->holdings.next;
        weivel_list_remove(link);
        free(WEIVEL_RECORD_OF(link, weivel_holding, by_registration));
    }
    free(registration);
}

static void free_slot(weivel_slot *slot) {
    if (slot->registration != NULL) {
        free_registration(slot->registration);
    }
    while (!weivel_list_empty(&slot->waiters)) {
        weivel_link *link = slot->waiters.next;
        weivel_list_remove(link);
        weivel_waiting_free(WEIVEL_RECORD_OF(link, weivel_want, by_slot));
    }
    while (!weivel_list_empty(&slot->pollers)) {
        weivel_link *link = slot->pollers.next;
        weivel_list_remove(link);
        weivel_waiting_free(WEIVEL_RECORD_OF(link, weivel_want, by_slot));
    }
    while (!weivel_list_empty(&slot->watchers)) {
        weivel_link *link = slot->watchers.next;
        weivel_list_remove(link);
        free(WEIVEL_RECORD_OF(link, weivel_watch, by_slot));
    }
    free(slot);
}

void weivel_registry_destroy(weivel_registry *registry) {
    if (registry == NULL) {
        return;
    }
    if (registry->slots != NULL) {
        while (!weivel_list_empty(&registry->all_slots)) {
            weivel_link *link = registry->all_slots.next;
            weivel_list_remove(link);
            free_slot(WEIVEL_RECORD_OF(link, weivel_slot, in_registry));
        }
    }
    weivel_table_destroy(registry->slots);
    weivel_table_destroy(registry->holdings);
    free(registry);
}

weivel_slot *weivel_registry_find(const weivel_registry *registry, const char *id) {
    return weivel_table_find_text(registry->slots, id);
}

weivel_slot *weivel_registry_slot(weivel_registry *registry, const char *id) {
    weivel_slot *slot = weivel_registry_find(registry, id);
    if (slot != NULL) {
        return slot;
    }
    size_t length = strlen(id);
    slot = weivel_memory_allocate_zeroed(1, sizeof *slot + length + 1);
    if (slot == NULL) {
        return NULL;
    }
    slot->id = (char *)(slot + 1);
    memcpy(slot->id, id, length + 1);
    weivel_list_init(&slot->waiters);
    weivel_list_init(&slot->pollers);
    weivel_list_init(&slot->watchers);
    if (weivel_table_add_text(registry->slots, slot->id, slot) != WEIVEL_STATUS_OK) {
        free(slot);
        return NULL;
    }
    weivel_list_append(&registry->all_slots, &slot->in_registry);
    registry->slot_count++;
    return slot;
}

void weivel_registry_release_slot(weivel_registry *registry, weivel_slot *slot) {
    if (slot->registration != NULL || !weivel_list_empty(&slot->waiters) || !weivel_list_empty(&slot->pollers) ||
        !weivel_list_empty(&slot->watchers)) {
        return;
    }
    weivel_table_remove_text(registry->slots, slot->id);
    weivel_list_remove(&slot->in_registry);
    registry->slot_count--;
    free(slot);
}

const char *weivel_registry_owner_name(const weivel_registration *registration) {
    return registration->owner == NULL ? "the host" : registration->owner->identity;
}

bool weivel_registry_accepts(const char *provider, const weivel_registration *registration) {
    if (provider == NULL) {
        return true;
    }
    return registration->owner != NULL && strcmp(provider, registration->owner->identity) == 0;
}

static weivel_holding_key holding_key(const weivel_registration *registration, const weivel_plugin *plugin) {
    weivel_holding_key key;
    memset(&key, 0, sizeof key);
    key.registration = registration->handle;
    key.plugin = plugin->handle;
    return key;
}

weivel_status weivel_registry_hold(weivel_core *core, weivel_registration *registration, weivel_plugin *plugin) {
    weivel_holding_key key = holding_key(registration, plugin);
    if (weivel_table_find(core->registry->holdings, &key, sizeof key) != NULL) {
        return WEIVEL_STATUS_OK;
    }
    if (core->registry->fail_next_holding) {
        core->registry->fail_next_holding = false;
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_holding *holding = weivel_memory_allocate_zeroed(1, sizeof *holding);
    if (holding == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    holding->key = key;
    holding->registration = registration;
    holding->plugin = plugin;
    if (weivel_table_add(core->registry->holdings, &holding->key, sizeof holding->key, holding) != WEIVEL_STATUS_OK) {
        free(holding);
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_list_append(&registration->holdings, &holding->by_registration);
    weivel_list_append(&plugin->holdings, &holding->by_plugin);
    core->registry->holding_count++;
    return WEIVEL_STATUS_OK;
}

static void revoke_holding(weivel_core *core, weivel_holding *holding) {
    weivel_revoked_event revoked = {holding->plugin->handle, holding->registration->handle};
    weivel_events_push_revoked(core->events, &revoked);
    weivel_table_remove(core->registry->holdings, &holding->key, sizeof holding->key);
    weivel_list_remove(&holding->by_registration);
    weivel_list_remove(&holding->by_plugin);
    core->registry->holding_count--;
    free(holding);
}

static void log_unheld(weivel_core *core, const char *id) {
    char *text =
        weivel_text_format("memory ran out recording who holds service \"%s\", so it was not handed over", id);
    weivel_core_log(core, WEIVEL_LOG_ERROR,
                    text == NULL ? "memory ran out recording who holds a service, so it was not handed over" : text);
    free(text);
}

static void tell_watchers(weivel_core *core, weivel_registration *registration, uint32_t change) {
    weivel_link *head = &registration->slot->watchers;
    for (weivel_link *link = head->next; link != head; link = link->next) {
        weivel_watch *watch = WEIVEL_RECORD_OF(link, weivel_watch, by_slot);
        if (!weivel_registry_accepts(watch->provider, registration)) {
            continue;
        }
        if (change == WEIVEL_CHANGE_REGISTERED &&
            weivel_registry_hold(core, registration, watch->consumer) != WEIVEL_STATUS_OK) {
            log_unheld(core, registration->slot->id);
            continue;
        }
        weivel_watch_event heard = {watch->handle, watch->consumer->handle, registration->handle, change};
        weivel_events_push_watch(core->events, &heard);
    }
}

void weivel_registry_unregister(weivel_core *core, weivel_registration *registration) {
    registration->slot->registration = NULL;
    weivel_list_remove(&registration->by_owner);
    while (!weivel_list_empty(&registration->holdings)) {
        revoke_holding(core, WEIVEL_RECORD_OF(registration->holdings.next, weivel_holding, by_registration));
    }
    tell_watchers(core, registration, WEIVEL_CHANGE_UNREGISTERED);
    weivel_handles_release(core->handles, registration->handle, WEIVEL_RECORD_REGISTRATION);
    weivel_slot *slot = registration->slot;
    free(registration);
    weivel_registry_release_slot(core->registry, slot);
}

static void stop_watch(weivel_core *core, weivel_watch *watch) {
    weivel_list_remove(&watch->by_slot);
    weivel_list_remove(&watch->by_consumer);
    weivel_handles_release(core->handles, watch->handle, WEIVEL_RECORD_WATCH);
    weivel_slot *slot = watch->slot;
    free(watch);
    weivel_registry_release_slot(core->registry, slot);
}

void weivel_registry_strip(weivel_core *core, weivel_plugin *plugin) {
    while (!weivel_list_empty(&plugin->watches)) {
        stop_watch(core, WEIVEL_RECORD_OF(plugin->watches.next, weivel_watch, by_consumer));
    }
    while (!weivel_list_empty(&plugin->registrations)) {
        weivel_registry_unregister(core, WEIVEL_RECORD_OF(plugin->registrations.next, weivel_registration, by_owner));
    }
    while (!weivel_list_empty(&plugin->wants)) {
        weivel_want *want = WEIVEL_RECORD_OF(plugin->wants.next, weivel_want, by_consumer);
        weivel_reach stopped;
        weivel_reach_stopped(want->slot->id, &stopped);
        weivel_waiting_reject(core, want, WEIVEL_REFUSAL_STOPPED, stopped.detail, stopped.fix);
        weivel_reach_dispose(&stopped);
    }
    while (!weivel_list_empty(&plugin->holdings)) {
        revoke_holding(core, WEIVEL_RECORD_OF(plugin->holdings.next, weivel_holding, by_plugin));
    }
}

static bool is_well_known(const weivel_core *core, const char *id) {
    for (uint32_t index = 0; index < core->well_known.count; index++) {
        if (strcmp(core->well_known.items[index], id) == 0) {
            return true;
        }
    }
    return false;
}

static bool in_namespace(const weivel_plugin *plugin, const char *id) {
    const char *separator = strchr(id, ':');
    if (separator == NULL || separator == id || separator[1] == '\0') {
        return false;
    }
    const char *namespace_name = plugin->manifest.value.import_name;
    size_t length = (size_t)(separator - id);
    return strlen(namespace_name) == length && memcmp(namespace_name, id, length) == 0;
}

/** Every id any plugin may register, joined by ", ", or "(none)"; NULL when memory runs out. */
static char *join_well_known(const weivel_core *core) {
    size_t length = core->well_known.count == 0 ? strlen("(none)") : 0;
    for (uint32_t index = 0; index < core->well_known.count; index++) {
        length += strlen(core->well_known.items[index]) + (index == 0 ? 0 : 2);
    }
    char *text = weivel_memory_allocate(length + 1);
    if (text == NULL) {
        return NULL;
    }
    char *cursor = text;
    if (core->well_known.count == 0) {
        memcpy(cursor, "(none)", 6);
        cursor += 6;
    }
    for (uint32_t index = 0; index < core->well_known.count; index++) {
        if (index > 0) {
            memcpy(cursor, ", ", 2);
            cursor += 2;
        }
        size_t part = strlen(core->well_known.items[index]);
        memcpy(cursor, core->well_known.items[index], part);
        cursor += part;
    }
    *cursor = '\0';
    return text;
}

static weivel_status refuse_namespace(weivel_core *core, const weivel_plugin *plugin, const char *id) {
    const char *name = id;
    for (const char *cursor = id; *cursor != '\0'; cursor++) {
        if (*cursor == ':') {
            name = cursor + 1;
        }
    }
    char *known = join_well_known(core);
    char *detail =
        weivel_text_format("cannot register service \"%s\", which is outside this plugin's namespace", id);
    char *fix = known == NULL ? NULL
                              : weivel_text_format("register it as \"%s:%s\", or as one of the well-known ids: %s",
                                                   plugin->manifest.value.import_name, name, known);
    weivel_status status = WEIVEL_STATUS_OUT_OF_MEMORY;
    if (detail != NULL && fix != NULL) {
        weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_OUTSIDE_NAMESPACE, detail, fix);
        status = WEIVEL_STATUS_REFUSED;
    }
    free(fix);
    free(detail);
    free(known);
    return status;
}

static weivel_status refuse_taken(weivel_core *core, const weivel_registration *holder) {
    char *detail = weivel_text_format("service \"%s\" is already registered by %s", holder->slot->id,
                                      weivel_registry_owner_name(holder));
    if (detail == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_SERVICE_TAKEN, detail,
                               "disable one of the two plugins, or have each register its own namespaced id");
    free(detail);
    return WEIVEL_STATUS_REFUSED;
}

/** What a plugin's manifest advertises for an id it provides, NULL when it advertises nothing for it. */
static const weivel_numbers *advertised_by(const weivel_plugin *owner, const char *id) {
    const weivel_declaration *declaration = owner == NULL ? NULL : weivel_table_find_text(owner->declared, id);
    if (declaration == NULL || declaration->offered == NULL || declaration->offered->attributes.count == 0) {
        return NULL;
    }
    return &declaration->offered->attributes;
}

static weivel_status add_registration(weivel_core *core, weivel_plugin *owner, weivel_slot *slot,
                                      weivel_handle *handle) {
    weivel_registration *registration = weivel_memory_allocate_zeroed(1, sizeof *registration);
    if (registration == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_status status =
        weivel_handles_allocate(core->handles, WEIVEL_RECORD_REGISTRATION, registration, &registration->handle);
    if (status != WEIVEL_STATUS_OK) {
        free(registration);
        return status;
    }
    registration->slot = slot;
    registration->owner = owner;
    registration->advertised = advertised_by(owner, slot->id);
    weivel_list_init(&registration->holdings);
    weivel_list_init(&registration->by_owner);
    if (owner != NULL) {
        weivel_list_append(&owner->registrations, &registration->by_owner);
    }
    slot->registration = registration;
    *handle = registration->handle;
    weivel_waiting_arrival(core, slot);
    tell_watchers(core, registration, WEIVEL_CHANGE_REGISTERED);
    return WEIVEL_STATUS_OK;
}

static weivel_status register_in_slot(weivel_core *core, weivel_plugin *owner, const char *id,
                                      weivel_handle *handle) {
    weivel_slot *slot = weivel_registry_slot(core->registry, id);
    if (slot == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_status status = add_registration(core, owner, slot, handle);
    if (status != WEIVEL_STATUS_OK) {
        weivel_registry_release_slot(core->registry, slot);
    }
    return status;
}

static weivel_status register_for_host(weivel_core *core, const char *id, weivel_handle *handle) {
    weivel_slot *held = weivel_registry_find(core->registry, id);
    if (held != NULL && held->registration != NULL) {
        weivel_registry_unregister(core, held->registration);
    }
    return register_in_slot(core, NULL, id, handle);
}

static weivel_status register_for_plugin(weivel_core *core, weivel_handle owner, const char *id,
                                         weivel_handle *handle) {
    weivel_plugin *plugin;
    weivel_status status = weivel_core_opened_plugin(core, owner, &plugin);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    if (plugin->state != WEIVEL_PLUGIN_OPEN) {
        weivel_reach_log_late(core, plugin, "registration", id);
        return WEIVEL_STATUS_REFUSED;
    }
    if (!is_well_known(core, id) && !in_namespace(plugin, id)) {
        return refuse_namespace(core, plugin, id);
    }
    const weivel_slot *held = weivel_registry_find(core->registry, id);
    if (held != NULL && held->registration != NULL) {
        return refuse_taken(core, held->registration);
    }
    status = register_in_slot(core, plugin, id, handle);
    if (status == WEIVEL_STATUS_OK) {
        weivel_ledger_note(core, plugin, WEIVEL_RELATIONSHIP_SERVICE_PROVIDED, id);
    }
    return status;
}

static bool is_id(const char *id) {
    return id != NULL && id[0] != '\0';
}

weivel_status weivel_register_service(weivel_core *core, weivel_handle owner, const char *id,
                                      weivel_handle *registration, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (registration == NULL || !is_id(id)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_register_service was given no service id, or no place to write the registration",
            "pass a non-empty service id and a pointer to a weivel_handle");
        if (registration != NULL) {
            *registration = 0;
        }
    } else {
        *registration = 0;
        status = owner == WEIVEL_HOST ? register_for_host(core, id, registration)
                                      : register_for_plugin(core, owner, id, registration);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

static weivel_status refuse_stale(weivel_core *core, weivel_handle handle) {
    char detail[128];
    snprintf(detail, sizeof detail, "the handle %" PRIu64 " names no registration, watch or want on this core",
             handle);
    weivel_events_push_refusal(
        core->events, WEIVEL_REFUSAL_STALE_HANDLE, detail,
        "pass a handle a registration, a watch or a want answered, and only until what it names has ended");
    return WEIVEL_STATUS_REFUSED;
}

static weivel_status end_handle(weivel_core *core, weivel_handle handle) {
    void *record;
    if (weivel_handles_resolve(core->handles, handle, WEIVEL_RECORD_REGISTRATION, &record) == WEIVEL_STATUS_OK) {
        weivel_registry_unregister(core, record);
    } else if (weivel_handles_resolve(core->handles, handle, WEIVEL_RECORD_WATCH, &record) == WEIVEL_STATUS_OK) {
        stop_watch(core, record);
    } else if (weivel_handles_resolve(core->handles, handle, WEIVEL_RECORD_WANT, &record) == WEIVEL_STATUS_OK) {
        weivel_waiting_withdraw(core, record);
    } else if (!weivel_capabilities_end_watch(core, handle)) {
        return refuse_stale(core, handle);
    }
    return WEIVEL_STATUS_OK;
}

weivel_status weivel_unregister(weivel_core *core, weivel_handle handle, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status = end_handle(core, handle);
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

/** Checks reach for an attempt that needs no refusal texts, since the check reported them already. */
static weivel_reach reach_without_texts(weivel_core *core, weivel_plugin *plugin, const char *id) {
    weivel_reach reach;
    weivel_reach_check(core, plugin, id, &reach);
    weivel_reach_dispose(&reach);
    return reach;
}

static weivel_status get_service(weivel_core *core, weivel_handle consumer, const char *id,
                                 weivel_handle *implementation) {
    weivel_plugin *plugin;
    weivel_status status = weivel_core_opened_plugin(core, consumer, &plugin);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    weivel_reach reach = reach_without_texts(core, plugin, id);
    if (reach.code != WEIVEL_REFUSAL_NONE) {
        return WEIVEL_STATUS_REFUSED;
    }
    weivel_ledger_note(core, plugin, WEIVEL_RELATIONSHIP_SERVICE_CONSUMED, id);
    weivel_slot *slot = weivel_registry_find(core->registry, id);
    if (slot == NULL || slot->registration == NULL) {
        return WEIVEL_STATUS_NONE;
    }
    status = weivel_registry_hold(core, slot->registration, plugin);
    if (status == WEIVEL_STATUS_OK) {
        *implementation = slot->registration->handle;
    }
    return status;
}

weivel_status weivel_get_service(weivel_core *core, weivel_handle plugin, const char *id,
                                 weivel_handle *implementation, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (implementation == NULL || !is_id(id)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_get_service was given no service id, or no place to write the implementation",
            "pass a non-empty service id and a pointer to a weivel_handle");
        if (implementation != NULL) {
            *implementation = 0;
        }
    } else {
        *implementation = 0;
        status = get_service(core, plugin, id, implementation);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

static weivel_status add_watch(weivel_core *core, weivel_plugin *plugin, const char *id, const char *provider,
                               weivel_handle *handle) {
    weivel_slot *slot = weivel_registry_slot(core->registry, id);
    if (slot == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_watch *watch = weivel_memory_allocate_zeroed(1, sizeof *watch);
    weivel_status status = watch == NULL ? WEIVEL_STATUS_OUT_OF_MEMORY
                                         : weivel_handles_allocate(core->handles, WEIVEL_RECORD_WATCH, watch,
                                                                   &watch->handle);
    if (status != WEIVEL_STATUS_OK) {
        free(watch);
        weivel_registry_release_slot(core->registry, slot);
        return status;
    }
    watch->slot = slot;
    watch->consumer = plugin;
    watch->provider = provider;
    weivel_list_append(&slot->watchers, &watch->by_slot);
    weivel_list_append(&plugin->watches, &watch->by_consumer);
    *handle = watch->handle;
    return WEIVEL_STATUS_OK;
}

static weivel_status watch_service(weivel_core *core, weivel_handle consumer, const char *id, weivel_handle *handle) {
    weivel_plugin *plugin;
    weivel_status status = weivel_core_opened_plugin(core, consumer, &plugin);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    weivel_reach reach = reach_without_texts(core, plugin, id);
    if (reach.code != WEIVEL_REFUSAL_NONE) {
        return WEIVEL_STATUS_REFUSED;
    }
    if (reach.late) {
        weivel_reach_log_late(core, plugin, "watch", id);
        return WEIVEL_STATUS_REFUSED;
    }
    status = add_watch(core, plugin, id, reach.provider, handle);
    if (status == WEIVEL_STATUS_OK) {
        weivel_ledger_note(core, plugin, WEIVEL_RELATIONSHIP_SERVICE_CONSUMED, id);
    }
    return status;
}

weivel_status weivel_watch_service(weivel_core *core, weivel_handle plugin, const char *id, weivel_handle *watch,
                                   uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (watch == NULL || !is_id(id)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_watch_service was given no service id, or no place to write the watch",
            "pass a non-empty service id and a pointer to a weivel_handle");
        if (watch != NULL) {
            *watch = 0;
        }
    } else {
        *watch = 0;
        status = watch_service(core, plugin, id, watch);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

/** Copies `count` ids into one allocation laid out as the pointer array followed by the texts. */
static weivel_status fill_list(const char *const *ids, uint32_t count, weivel_service_list *list) {
    size_t total = (size_t)count * sizeof(const char *);
    for (uint32_t index = 0; index < count; index++) {
        total += strlen(ids[index]) + 1;
    }
    void *storage = weivel_memory_allocate(total == 0 ? 1 : total);
    if (storage == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    const char **items = storage;
    char *cursor = (char *)storage + (size_t)count * sizeof(const char *);
    for (uint32_t index = 0; index < count; index++) {
        size_t length = strlen(ids[index]) + 1;
        memcpy(cursor, ids[index], length);
        items[index] = cursor;
        cursor += length;
    }
    list->ids.items = count == 0 ? NULL : items;
    list->ids.count = count;
    list->storage = storage;
    return WEIVEL_STATUS_OK;
}

typedef bool (*weivel_listed)(const weivel_core *core, const weivel_plugin *plugin, uint32_t index,
                              const char **id);

static bool reachable_and_registered(const weivel_core *core, const weivel_plugin *plugin, uint32_t index,
                                     const char **id) {
    const weivel_declaration *declaration = &plugin->declarations[index];
    const weivel_slot *slot = weivel_registry_find(core->registry, declaration->id);
    *id = declaration->id;
    return slot != NULL && slot->registration != NULL;
}

static bool consumed_without_provider(const weivel_core *core, const weivel_plugin *plugin, uint32_t index,
                                      const char **id) {
    (void)core;
    const weivel_service_want *want = &plugin->manifest.value.services.consumes.items[index];
    *id = want->id;
    return want->provider == NULL && strchr(want->id, ':') != NULL;
}

static weivel_status list_ids(const weivel_core *core, const weivel_plugin *plugin, uint32_t candidates,
                              weivel_listed listed, weivel_service_list *list) {
    const char **ids = weivel_memory_allocate((candidates == 0 ? 1 : candidates) * sizeof *ids);
    if (ids == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    uint32_t count = 0;
    for (uint32_t index = 0; index < candidates; index++) {
        if (listed(core, plugin, index, &ids[count])) {
            count++;
        }
    }
    weivel_status status = fill_list(ids, count, list);
    free((void *)ids);
    return status;
}

static bool is_empty_list(const weivel_service_list *list) {
    return list != NULL && weivel_sized_size(list) >= WEIVEL_SERVICE_LIST_FIRST_SIZE && list->storage == NULL;
}

static weivel_status answer_list(weivel_core *core, weivel_handle plugin, weivel_service_list *list, bool unbound) {
    if (!is_empty_list(list)) {
        return weivel_core_refuse_malformed(
            core, "a service list was missing, too small, or still holding a list",
            "pass a weivel_service_list initialised with WEIVEL_INIT, or released since its last use");
    }
    weivel_plugin *record = NULL;
    weivel_status status = unbound ? weivel_core_plugin(core, plugin, &record)
                                   : weivel_core_opened_plugin(core, plugin, &record);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    weivel_service_list answer;
    memset(&answer, 0, sizeof answer);
    answer.size = (uint32_t)sizeof answer;
    status = unbound ? list_ids(core, record, record->manifest.value.services.consumes.count,
                                consumed_without_provider, &answer)
                     : list_ids(core, record, record->declaration_count, reachable_and_registered, &answer);
    if (status == WEIVEL_STATUS_OK) {
        weivel_sized_write(list, &answer, sizeof answer);
    }
    return status;
}

static weivel_status list_call(weivel_core *core, weivel_handle plugin, weivel_service_list *list, bool unbound,
                               uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status = answer_list(core, plugin, list, unbound);
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

weivel_status weivel_service_ids(weivel_core *core, weivel_handle plugin, weivel_service_list *list,
                                 uint32_t *pending_events) {
    return list_call(core, plugin, list, false, pending_events);
}

weivel_status weivel_unbound_services(weivel_core *core, weivel_handle plugin, weivel_service_list *list,
                                      uint32_t *pending_events) {
    return list_call(core, plugin, list, true, pending_events);
}

weivel_status weivel_service_list_release(weivel_service_list *list) {
    if (list != NULL && list->size == 0) {
        return WEIVEL_STATUS_OK;
    }
    if (list == NULL || list->size < WEIVEL_SERVICE_LIST_FIRST_SIZE) {
        return WEIVEL_STATUS_MALFORMED;
    }
    free(list->storage);
    weivel_service_list empty;
    memset(&empty, 0, sizeof empty);
    weivel_sized_write(list, &empty, sizeof empty);
    return WEIVEL_STATUS_OK;
}
