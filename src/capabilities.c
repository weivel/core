#include "capabilities.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "sized.h"
#include "text.h"

weivel_capabilities *weivel_capabilities_create(void) {
    weivel_capabilities *capabilities = calloc(1, sizeof *capabilities);
    if (capabilities == NULL) {
        return NULL;
    }
    weivel_list_init(&capabilities->all);
    capabilities->by_id = weivel_table_create();
    capabilities->holdings = weivel_table_create();
    if (capabilities->by_id == NULL || capabilities->holdings == NULL) {
        weivel_table_destroy(capabilities->by_id);
        weivel_table_destroy(capabilities->holdings);
        free(capabilities);
        return NULL;
    }
    return capabilities;
}

static void free_provision(weivel_provision *provision) {
    while (!weivel_list_empty(&provision->holdings)) {
        weivel_link *link = provision->holdings.next;
        weivel_list_remove(link);
        free(WEIVEL_RECORD_OF(link, weivel_capability_holding, by_provision));
    }
    free(provision);
}

static void free_capability(weivel_capability *capability) {
    while (!weivel_list_empty(&capability->provided)) {
        weivel_link *link = capability->provided.next;
        weivel_list_remove(link);
        free_provision(WEIVEL_RECORD_OF(link, weivel_provision, by_capability));
    }
    while (!weivel_list_empty(&capability->watchers)) {
        weivel_link *link = capability->watchers.next;
        weivel_list_remove(link);
        free(WEIVEL_RECORD_OF(link, weivel_capability_watch, by_capability));
    }
    free(capability);
}

void weivel_capabilities_destroy(weivel_capabilities *capabilities) {
    if (capabilities == NULL) {
        return;
    }
    while (!weivel_list_empty(&capabilities->all)) {
        weivel_link *link = capabilities->all.next;
        weivel_list_remove(link);
        free_capability(WEIVEL_RECORD_OF(link, weivel_capability, in_capabilities));
    }
    weivel_table_destroy(capabilities->by_id);
    weivel_table_destroy(capabilities->holdings);
    free(capabilities);
}

static weivel_capability *find_capability(const weivel_capabilities *capabilities, const char *id) {
    return weivel_table_find_text(capabilities->by_id, id);
}

static weivel_capability *capability_of(weivel_capabilities *capabilities, const char *id) {
    weivel_capability *capability = find_capability(capabilities, id);
    if (capability != NULL) {
        return capability;
    }
    size_t length = strlen(id) + 1;
    capability = malloc(sizeof *capability + length);
    if (capability == NULL) {
        return NULL;
    }
    capability->id = (char *)(capability + 1);
    memcpy(capability->id, id, length);
    weivel_list_init(&capability->provided);
    weivel_list_init(&capability->verified);
    weivel_list_init(&capability->watchers);
    if (weivel_table_add_text(capabilities->by_id, capability->id, capability) != WEIVEL_STATUS_OK) {
        free(capability);
        return NULL;
    }
    weivel_list_append(&capabilities->all, &capability->in_capabilities);
    capabilities->count++;
    return capability;
}

static void release_capability(weivel_capabilities *capabilities, weivel_capability *capability) {
    if (!weivel_list_empty(&capability->provided) || !weivel_list_empty(&capability->watchers)) {
        return;
    }
    weivel_table_remove_text(capabilities->by_id, capability->id);
    weivel_list_remove(&capability->in_capabilities);
    capabilities->count--;
    free(capability);
}

static void tell_watchers(weivel_core *core, const weivel_provision *provision, uint32_t change) {
    weivel_capability *capability = provision->capability;
    for (weivel_link *link = capability->watchers.next; link != &capability->watchers; link = link->next) {
        const weivel_capability_watch *watch = WEIVEL_RECORD_OF(link, weivel_capability_watch, by_capability);
        weivel_capability_event heard;
        heard.watch = watch->handle;
        heard.plugin = watch->consumer->handle;
        heard.implementation = provision->handle;
        heard.provider = provision->provider->handle;
        heard.change = change;
        weivel_events_push_capability(core->events, &heard);
    }
}

static bool is_id(const char *id) {
    return id != NULL && id[0] != '\0';
}

static weivel_status refuse_provided_twice(weivel_core *core, const char *id) {
    char *detail = weivel_text_format("provided capability \"%s\" twice", id);
    if (detail == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_PROVIDED_TWICE, detail,
                               "provide each capability once while the plugin activates");
    free(detail);
    return WEIVEL_STATUS_REFUSED;
}

static weivel_status add_provision(weivel_core *core, weivel_plugin *plugin, const char *id,
                                   weivel_handle *implementation) {
    weivel_capabilities *capabilities = core->capabilities;
    weivel_capability *capability = capability_of(capabilities, id);
    if (capability == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_provision *provision = calloc(1, sizeof *provision);
    weivel_status status = provision == NULL ? WEIVEL_STATUS_OUT_OF_MEMORY
                                             : weivel_handles_allocate(core->handles, WEIVEL_RECORD_PROVISION,
                                                                       provision, &provision->handle);
    if (status == WEIVEL_STATUS_OK) {
        status = weivel_table_add_text(plugin->provided, capability->id, provision);
        if (status != WEIVEL_STATUS_OK) {
            weivel_handles_release(core->handles, provision->handle, WEIVEL_RECORD_PROVISION);
        }
    }
    if (status != WEIVEL_STATUS_OK) {
        free(provision);
        release_capability(capabilities, capability);
        return status;
    }
    provision->capability = capability;
    provision->provider = plugin;
    weivel_list_init(&provision->in_verified);
    weivel_list_init(&provision->holdings);
    weivel_list_append(&capability->provided, &provision->by_capability);
    weivel_list_append(&plugin->provisions, &provision->by_provider);
    capabilities->provision_count++;
    *implementation = provision->handle;
    weivel_ledger_note(core, plugin, WEIVEL_RELATIONSHIP_CAPABILITY_PROVIDED, capability->id);
    return WEIVEL_STATUS_OK;
}

static weivel_status provide(weivel_core *core, weivel_handle provider, const char *id,
                             weivel_handle *implementation) {
    weivel_plugin *plugin;
    weivel_status status = weivel_core_opened_plugin(core, provider, &plugin);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    if (plugin->state != WEIVEL_PLUGIN_OPEN) {
        weivel_reach_log_late_capability(core, plugin, "provision", id);
        return WEIVEL_STATUS_REFUSED;
    }
    if (weivel_table_find_text(plugin->provided, id) != NULL) {
        return refuse_provided_twice(core, id);
    }
    return add_provision(core, plugin, id, implementation);
}

weivel_status weivel_provide_capability(weivel_core *core, weivel_handle plugin, const char *id,
                                        weivel_handle *implementation, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (implementation == NULL || !is_id(id)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_provide_capability was given no capability id, or no place to write the implementation",
            "pass a non-empty capability id and a pointer to a weivel_handle");
        if (implementation != NULL) {
            *implementation = 0;
        }
    } else {
        *implementation = 0;
        status = provide(core, plugin, id, implementation);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

/** The ids a plugin's activation left out or added, joined by ", ", and the first of them. */
typedef struct weivel_mismatch {
    const char *first;
    char *joined;
} weivel_mismatch;

static void note_mismatch(weivel_mismatch *mismatch, size_t *used, size_t capacity, const char *id) {
    if (mismatch->first == NULL) {
        mismatch->first = id;
    }
    if (mismatch->joined != NULL) {
        *used += (size_t)snprintf(mismatch->joined + *used, capacity - *used, "%s%s", *used == 0 ? "" : ", ", id);
    }
}

/** Fills `mismatch` with the declared ids nothing provided; `capacity` 0 only finds the first and the length. */
static size_t find_missing(const weivel_plugin *plugin, weivel_mismatch *mismatch, size_t capacity) {
    const weivel_strings *declared = &plugin->manifest.value.capabilities.provides;
    size_t used = 0;
    size_t length = 0;
    for (uint32_t index = 0; index < declared->count; index++) {
        if (weivel_table_find_text(plugin->provided, declared->items[index]) == NULL) {
            length += strlen(declared->items[index]) + 2;
            note_mismatch(mismatch, &used, capacity, declared->items[index]);
        }
    }
    return length;
}

/** Fills `mismatch` with the provided ids the manifest does not declare, in the order provided. */
static size_t find_extra(const weivel_plugin *plugin, weivel_mismatch *mismatch, size_t capacity) {
    size_t used = 0;
    size_t length = 0;
    for (const weivel_link *link = plugin->provisions.next; link != &plugin->provisions; link = link->next) {
        const char *id = WEIVEL_RECORD_OF(link, weivel_provision, by_provider)->capability->id;
        if (weivel_table_find_text(plugin->capabilities_declared, id) == NULL) {
            length += strlen(id) + 2;
            note_mismatch(mismatch, &used, capacity, id);
        }
    }
    return length;
}

/** Joins what `missing` or extra ids a plugin's activation has into one refusal naming the first in its fix. */
static weivel_status refuse_mismatch(weivel_core *core, weivel_plugin *plugin, bool missing, size_t length) {
    const char *what =
        missing ? "capabilities declared but never provided: " : "capabilities provided but never declared: ";
    size_t prefix = strlen(what);
    char *detail = malloc(prefix + length + 1);
    if (detail == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    memcpy(detail, what, prefix + 1);
    weivel_mismatch mismatch = {NULL, detail + prefix};
    if (missing) {
        find_missing(plugin, &mismatch, length + 1);
    } else {
        find_extra(plugin, &mismatch, length + 1);
    }
    char *fix = missing ? weivel_text_format("provide \"%s\" while the plugin activates, or remove \"%s\" from the "
                                             "capabilities it provides in its manifest",
                                             mismatch.first, mismatch.first)
                        : weivel_text_format("add \"%s\" to the capabilities it provides in its manifest, or stop "
                                             "providing \"%s\"",
                                             mismatch.first, mismatch.first);
    if (fix == NULL) {
        free(detail);
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_ledger_note_refused_activation(core, plugin, WEIVEL_REFUSAL_ACTIVATION_MISMATCH, detail, fix);
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_ACTIVATION_MISMATCH, detail, fix);
    free(fix);
    free(detail);
    return WEIVEL_STATUS_REFUSED;
}

static void admit(weivel_core *core, weivel_plugin *plugin) {
    for (weivel_link *link = plugin->provisions.next; link != &plugin->provisions; link = link->next) {
        weivel_provision *provision = WEIVEL_RECORD_OF(link, weivel_provision, by_provider);
        if (provision->verified) {
            continue;
        }
        provision->verified = true;
        weivel_list_append(&provision->capability->verified, &provision->in_verified);
        tell_watchers(core, provision, WEIVEL_CAPABILITY_PROVIDED);
    }
}

static weivel_status refuse_not_open(weivel_core *core, const weivel_plugin *plugin) {
    bool released = plugin->state == WEIVEL_PLUGIN_RELEASED;
    char *detail = weivel_text_format("the plugin \"%s\" %s", plugin->manifest.value.import_name,
                             released ? "was released" : "is quarantined");
    if (detail == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_events_push_refusal(core->events, WEIVEL_REFUSAL_PLUGIN_STATE, detail,
                               released ? "register its manifest again, and use the handle that answers"
                                        : "open it again, and verify the activation that follows");
    free(detail);
    return WEIVEL_STATUS_REFUSED;
}

static weivel_status verify(weivel_core *core, weivel_handle handle) {
    weivel_plugin *plugin;
    weivel_status status = weivel_core_opened_plugin(core, handle, &plugin);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    if (plugin->state != WEIVEL_PLUGIN_OPEN) {
        return refuse_not_open(core, plugin);
    }
    weivel_mismatch missing = {NULL, NULL};
    size_t length = find_missing(plugin, &missing, 0);
    if (missing.first != NULL) {
        return refuse_mismatch(core, plugin, true, length);
    }
    weivel_mismatch extra = {NULL, NULL};
    length = find_extra(plugin, &extra, 0);
    if (extra.first != NULL) {
        return refuse_mismatch(core, plugin, false, length);
    }
    admit(core, plugin);
    weivel_ledger_note_verified(plugin);
    return WEIVEL_STATUS_OK;
}

weivel_status weivel_verify_activation(weivel_core *core, weivel_handle plugin, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status = verify(core, plugin);
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

static weivel_capability_holding_key holding_key(const weivel_provision *provision, const weivel_plugin *plugin) {
    weivel_capability_holding_key key;
    memset(&key, 0, sizeof key);
    key.provision = provision->handle;
    key.plugin = plugin->handle;
    return key;
}

/** Records that `plugin` holds `provision`, once however often; `*created` says whether this call made it. */
static weivel_capability_holding *hold(weivel_core *core, weivel_provision *provision, weivel_plugin *plugin,
                                       bool *created) {
    weivel_capabilities *capabilities = core->capabilities;
    weivel_capability_holding_key key = holding_key(provision, plugin);
    weivel_capability_holding *holding = weivel_table_find(capabilities->holdings, &key, sizeof key);
    *created = false;
    if (holding != NULL) {
        return holding;
    }
    if (core->registry->fail_next_holding) {
        core->registry->fail_next_holding = false;
        return NULL;
    }
    holding = calloc(1, sizeof *holding);
    if (holding == NULL) {
        return NULL;
    }
    holding->key = key;
    holding->provision = provision;
    holding->plugin = plugin;
    if (weivel_table_add(capabilities->holdings, &holding->key, sizeof holding->key, holding) != WEIVEL_STATUS_OK) {
        free(holding);
        return NULL;
    }
    weivel_list_append(&provision->holdings, &holding->by_provision);
    weivel_list_append(&plugin->capability_holdings, &holding->by_plugin);
    capabilities->holding_count++;
    *created = true;
    return holding;
}

static void end_holding(weivel_core *core, weivel_capability_holding *holding) {
    weivel_table_remove(core->capabilities->holdings, &holding->key, sizeof holding->key);
    weivel_list_remove(&holding->by_provision);
    weivel_list_remove(&holding->by_plugin);
    core->capabilities->holding_count--;
    free(holding);
}

static void revoke_holding(weivel_core *core, weivel_capability_holding *holding) {
    weivel_revoked_event revoked = {holding->plugin->handle, holding->provision->handle};
    weivel_events_push_revoked(core->events, &revoked);
    end_holding(core, holding);
}

/**
 * Records that `plugin` holds every verified implementation of `capability`, or, when one cannot be recorded, ends
 * every holding this call made, so nothing is handed over that would never be revoked.
 */
static weivel_status hold_all(weivel_core *core, const weivel_capability *capability, weivel_plugin *plugin,
                              uint32_t count) {
    weivel_capability_holding **made = malloc(count == 0 ? 1 : (size_t)count * sizeof *made);
    if (made == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    uint32_t made_count = 0;
    weivel_status status = WEIVEL_STATUS_OK;
    for (const weivel_link *link = capability == NULL ? NULL : capability->verified.next;
         status == WEIVEL_STATUS_OK && link != NULL && link != &capability->verified; link = link->next) {
        bool created;
        weivel_capability_holding *holding =
            hold(core, WEIVEL_RECORD_OF(link, weivel_provision, in_verified), plugin, &created);
        if (holding == NULL) {
            status = WEIVEL_STATUS_OUT_OF_MEMORY;
        } else if (created) {
            made[made_count++] = holding;
        }
    }
    while (status != WEIVEL_STATUS_OK && made_count > 0) {
        end_holding(core, made[--made_count]);
    }
    free((void *)made);
    return status;
}

/** Fills `answer` with the verified implementations of `capability`, none when it is NULL. */
static weivel_status fill_all(const weivel_capability *capability, weivel_capability_list *answer) {
    uint32_t count = 0;
    for (const weivel_link *link = capability == NULL ? NULL : capability->verified.next;
         link != NULL && link != &capability->verified; link = link->next) {
        count++;
    }
    weivel_capability_implementation *items = malloc(count == 0 ? 1 : (size_t)count * sizeof *items);
    if (items == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    uint32_t index = 0;
    for (const weivel_link *link = capability == NULL ? NULL : capability->verified.next;
         link != NULL && link != &capability->verified; link = link->next) {
        const weivel_provision *provision = WEIVEL_RECORD_OF(link, weivel_provision, in_verified);
        items[index].size = (uint32_t)sizeof items[index];
        items[index].implementation = provision->handle;
        items[index].provider = provision->provider->handle;
        index++;
    }
    answer->items = count == 0 ? NULL : items;
    answer->count = count;
    answer->storage = items;
    return WEIVEL_STATUS_OK;
}

static bool is_empty_list(const weivel_capability_list *list) {
    return list != NULL && weivel_sized_size(list) >= WEIVEL_CAPABILITY_LIST_FIRST_SIZE && list->storage == NULL;
}

static weivel_status all(weivel_core *core, weivel_handle consumer, const char *id, weivel_capability_list *list) {
    weivel_plugin *plugin;
    weivel_status status = weivel_core_opened_plugin(core, consumer, &plugin);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    if (!weivel_reach_capability(core, plugin, id)) {
        return WEIVEL_STATUS_REFUSED;
    }
    weivel_capability_list answer;
    answer.size = (uint32_t)sizeof answer;
    answer.items = NULL;
    answer.count = 0;
    answer.storage = NULL;
    const weivel_capability *capability = find_capability(core->capabilities, id);
    status = fill_all(capability, &answer);
    if (status == WEIVEL_STATUS_OK) {
        status = hold_all(core, capability, plugin, answer.count);
        if (status != WEIVEL_STATUS_OK) {
            free(answer.storage);
        }
    }
    if (status == WEIVEL_STATUS_OK) {
        weivel_sized_write(list, &answer, sizeof answer);
    }
    return status;
}

weivel_status weivel_capability_all(weivel_core *core, weivel_handle plugin, const char *id,
                                    weivel_capability_list *list, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (!is_id(id) || !is_empty_list(list)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_capability_all was given no capability id, or a list missing, too small or still holding one",
            "pass a non-empty capability id and a weivel_capability_list initialised with WEIVEL_INIT, or released "
            "since its last use");
    } else {
        status = all(core, plugin, id, list);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

weivel_status weivel_capability_list_release(weivel_capability_list *list) {
    if (list != NULL && list->size == 0) {
        return WEIVEL_STATUS_OK;
    }
    if (list == NULL || list->size < WEIVEL_CAPABILITY_LIST_FIRST_SIZE) {
        return WEIVEL_STATUS_MALFORMED;
    }
    free(list->storage);
    weivel_capability_list empty;
    empty.size = (uint32_t)sizeof empty;
    empty.items = NULL;
    empty.count = 0;
    empty.storage = NULL;
    weivel_sized_write(list, &empty, sizeof empty);
    return WEIVEL_STATUS_OK;
}

static weivel_status add_watch(weivel_core *core, weivel_plugin *plugin, const char *id, weivel_handle *handle) {
    weivel_capabilities *capabilities = core->capabilities;
    weivel_capability *capability = capability_of(capabilities, id);
    if (capability == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_capability_watch *watch = calloc(1, sizeof *watch);
    weivel_status status = watch == NULL ? WEIVEL_STATUS_OUT_OF_MEMORY
                                         : weivel_handles_allocate(core->handles, WEIVEL_RECORD_CAPABILITY_WATCH,
                                                                   watch, &watch->handle);
    if (status != WEIVEL_STATUS_OK) {
        free(watch);
        release_capability(capabilities, capability);
        return status;
    }
    watch->capability = capability;
    watch->consumer = plugin;
    weivel_list_append(&capability->watchers, &watch->by_capability);
    weivel_list_append(&plugin->capability_watches, &watch->by_consumer);
    capabilities->watch_count++;
    *handle = watch->handle;
    return WEIVEL_STATUS_OK;
}

static weivel_status watch_capability(weivel_core *core, weivel_handle consumer, const char *id,
                                      weivel_handle *handle) {
    weivel_plugin *plugin;
    weivel_status status = weivel_core_opened_plugin(core, consumer, &plugin);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    if (!weivel_reach_capability(core, plugin, id)) {
        return WEIVEL_STATUS_REFUSED;
    }
    if (plugin->state != WEIVEL_PLUGIN_OPEN) {
        weivel_reach_log_late_capability(core, plugin, "watch", id);
        return WEIVEL_STATUS_REFUSED;
    }
    return add_watch(core, plugin, id, handle);
}

weivel_status weivel_watch_capability(weivel_core *core, weivel_handle plugin, const char *id, weivel_handle *watch,
                                      uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (watch == NULL || !is_id(id)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_watch_capability was given no capability id, or no place to write the watch",
            "pass a non-empty capability id and a pointer to a weivel_handle");
        if (watch != NULL) {
            *watch = 0;
        }
    } else {
        *watch = 0;
        status = watch_capability(core, plugin, id, watch);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

static void stop_watch(weivel_core *core, weivel_capability_watch *watch) {
    weivel_capability *capability = watch->capability;
    weivel_list_remove(&watch->by_capability);
    weivel_list_remove(&watch->by_consumer);
    weivel_handles_release(core->handles, watch->handle, WEIVEL_RECORD_CAPABILITY_WATCH);
    core->capabilities->watch_count--;
    free(watch);
    release_capability(core->capabilities, capability);
}

bool weivel_capabilities_end_watch(weivel_core *core, weivel_handle handle) {
    void *record;
    if (weivel_handles_resolve(core->handles, handle, WEIVEL_RECORD_CAPABILITY_WATCH, &record) != WEIVEL_STATUS_OK) {
        return false;
    }
    stop_watch(core, record);
    return true;
}

static void withdraw(weivel_core *core, weivel_provision *provision) {
    weivel_capability *capability = provision->capability;
    while (!weivel_list_empty(&provision->holdings)) {
        revoke_holding(core, WEIVEL_RECORD_OF(provision->holdings.next, weivel_capability_holding, by_provision));
    }
    if (provision->verified) {
        weivel_list_remove(&provision->in_verified);
        provision->verified = false;
        tell_watchers(core, provision, WEIVEL_CAPABILITY_WITHDRAWN);
    }
    weivel_list_remove(&provision->by_capability);
    weivel_list_remove(&provision->by_provider);
    weivel_table_remove_text(provision->provider->provided, capability->id);
    weivel_handles_release(core->handles, provision->handle, WEIVEL_RECORD_PROVISION);
    core->capabilities->provision_count--;
    free(provision);
    release_capability(core->capabilities, capability);
}

void weivel_capabilities_strip(weivel_core *core, weivel_plugin *plugin) {
    while (!weivel_list_empty(&plugin->provisions)) {
        withdraw(core, WEIVEL_RECORD_OF(plugin->provisions.next, weivel_provision, by_provider));
    }
    while (!weivel_list_empty(&plugin->capability_watches)) {
        stop_watch(core, WEIVEL_RECORD_OF(plugin->capability_watches.next, weivel_capability_watch, by_consumer));
    }
    while (!weivel_list_empty(&plugin->capability_holdings)) {
        revoke_holding(core,
                       WEIVEL_RECORD_OF(plugin->capability_holdings.next, weivel_capability_holding, by_plugin));
    }
}
