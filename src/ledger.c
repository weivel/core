#include "ledger.h"
#include "memory.h"

#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "sized.h"

void weivel_ids_clear(weivel_ids *ids) {
    for (uint32_t index = 0; index < ids->count; index++) {
        free(ids->items[index]);
    }
    free((void *)ids->items);
    weivel_table_destroy(ids->index);
    memset(ids, 0, sizeof *ids);
}

static bool grow_ids(weivel_ids *ids) {
    if (ids->capacity > UINT32_MAX / 2) {
        return false;
    }
    uint32_t capacity = ids->capacity == 0 ? 4 : ids->capacity * 2;
    char **items = weivel_memory_reallocate((void *)ids->items, (size_t)capacity * sizeof *items);
    if (items == NULL) {
        return false;
    }
    ids->items = items;
    ids->capacity = capacity;
    return true;
}

static weivel_status add_id(weivel_ids *ids, const char *id) {
    if (ids->index == NULL && (ids->index = weivel_table_create()) == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    if (weivel_table_find_text(ids->index, id) != NULL) {
        return WEIVEL_STATUS_OK;
    }
    if (ids->count == ids->capacity && !grow_ids(ids)) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    size_t length = strlen(id) + 1;
    char *copy = weivel_memory_allocate(length);
    if (copy == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    memcpy(copy, id, length);
    if (weivel_table_add_text(ids->index, copy, copy) != WEIVEL_STATUS_OK) {
        free(copy);
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    ids->items[ids->count++] = copy;
    return WEIVEL_STATUS_OK;
}

void weivel_kept_refusal_clear(weivel_kept_refusal *refusal) {
    free(refusal->detail);
    free(refusal->fix);
    memset(refusal, 0, sizeof *refusal);
}

bool weivel_kept_refusal_held(const weivel_kept_refusal *refusal) {
    return refusal->detail != NULL;
}

static char *copy_text(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = weivel_memory_allocate(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}

bool weivel_kept_refusal_copy(weivel_kept_refusal *kept, uint32_t code, const char *detail, const char *fix) {
    char *detail_copy = copy_text(detail);
    char *fix_copy = copy_text(fix);
    if (detail_copy == NULL || fix_copy == NULL) {
        free(detail_copy);
        free(fix_copy);
        return false;
    }
    kept->code = code;
    kept->detail = detail_copy;
    kept->fix = fix_copy;
    return true;
}

static weivel_ids *relationship_of(weivel_plugin *plugin, uint32_t relationship) {
    switch (relationship) {
    case WEIVEL_RELATIONSHIP_SERVICE_PROVIDED:
        return &plugin->services_provided;
    case WEIVEL_RELATIONSHIP_CAPABILITY_PROVIDED:
        return &plugin->capabilities_provided;
    default:
        return &plugin->services_consumed;
    }
}

void weivel_ledger_note(weivel_core *core, weivel_plugin *plugin, uint32_t relationship, const char *id) {
    if (add_id(relationship_of(plugin, relationship), id) != WEIVEL_STATUS_OK) {
        weivel_core_log(core, WEIVEL_LOG_ERROR, "memory ran out recording a relationship, so the ledger misses it");
    }
}

void weivel_ledger_begin_activation(weivel_plugin *plugin) {
    weivel_ids_clear(&plugin->services_provided);
    weivel_ids_clear(&plugin->capabilities_provided);
    weivel_ids_clear(&plugin->services_consumed);
    plugin->verified = false;
    weivel_kept_refusal_clear(&plugin->activation_refusal);
    weivel_kept_refusal_clear(&plugin->quarantine_reason);
}

void weivel_ledger_note_verified(weivel_plugin *plugin) {
    plugin->verified = true;
    weivel_kept_refusal_clear(&plugin->activation_refusal);
}

void weivel_ledger_note_refused_activation(weivel_core *core, weivel_plugin *plugin, uint32_t code,
                                           const char *detail, const char *fix) {
    weivel_kept_refusal_clear(&plugin->activation_refusal);
    if (!weivel_kept_refusal_copy(&plugin->activation_refusal, code, detail, fix)) {
        weivel_core_log(core, WEIVEL_LOG_ERROR,
                        "memory ran out recording why an activation was refused, so the ledger misses it");
    }
}

void weivel_ledger_free_plugin(weivel_plugin *plugin) {
    weivel_ids_clear(&plugin->services_provided);
    weivel_ids_clear(&plugin->capabilities_provided);
    weivel_ids_clear(&plugin->services_consumed);
    weivel_kept_refusal_clear(&plugin->activation_refusal);
    weivel_kept_refusal_clear(&plugin->quarantine_reason);
}

/**
 * Lays an answer out in one allocation: every array first, then every text. It runs twice over the same walk, once
 * with no `base` to measure, once to write, so the two can never disagree about a size.
 */
typedef struct weivel_layout {
    char *base;
    size_t array_cursor;
    size_t text_cursor;
} weivel_layout;

static void *reserve_array(weivel_layout *layout, size_t bytes) {
    void *array = layout->base == NULL ? NULL : layout->base + layout->array_cursor;
    layout->array_cursor += bytes;
    return array;
}

static const char *put_text(weivel_layout *layout, const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = NULL;
    if (layout->base != NULL) {
        copy = layout->base + layout->text_cursor;
        memcpy(copy, text, length);
    }
    layout->text_cursor += length;
    return copy;
}

static weivel_strings put_strings(weivel_layout *layout, const char *const *texts, uint32_t count) {
    const char **items = reserve_array(layout, (size_t)count * sizeof *items);
    for (uint32_t index = 0; index < count; index++) {
        const char *copy = put_text(layout, texts[index]);
        if (items != NULL) {
            items[index] = copy;
        }
    }
    weivel_strings strings;
    strings.items = count == 0 ? NULL : items;
    strings.count = count;
    return strings;
}

static weivel_strings put_ids(weivel_layout *layout, const weivel_ids *ids) {
    return put_strings(layout, (const char *const *)ids->items, ids->count);
}

static uint32_t status_of(const weivel_plugin *plugin) {
    switch (plugin->state) {
    case WEIVEL_PLUGIN_OPEN:
        return WEIVEL_PLUGIN_STATUS_ACTIVE;
    case WEIVEL_PLUGIN_QUARANTINED:
        return WEIVEL_PLUGIN_STATUS_QUARANTINED;
    case WEIVEL_PLUGIN_RELEASED:
        return WEIVEL_PLUGIN_STATUS_RELEASED;
    default:
        return WEIVEL_PLUGIN_STATUS_REGISTERED;
    }
}

static void put_refusal(weivel_layout *layout, const weivel_plugin *plugin, weivel_refusal *refusal) {
    memset(refusal, 0, sizeof *refusal);
    const weivel_kept_refusal *kept = weivel_kept_refusal_held(&plugin->quarantine_reason)
                                          ? &plugin->quarantine_reason
                                          : &plugin->activation_refusal;
    if (plugin->state != WEIVEL_PLUGIN_QUARANTINED || !weivel_kept_refusal_held(kept)) {
        return;
    }
    refusal->code = kept->code;
    refusal->detail = put_text(layout, kept->detail);
    refusal->fix = put_text(layout, kept->fix);
}

static void put_row(weivel_layout *layout, const weivel_plugin *plugin, weivel_ledger_row *row) {
    const weivel_manifest *manifest = &plugin->manifest.value;
    memset(row, 0, sizeof *row);
    row->size = (uint32_t)sizeof *row;
    row->plugin = plugin->handle;
    row->import_name = put_text(layout, manifest->import_name);
    row->identity = put_text(layout, plugin->identity);
    row->status = status_of(plugin);
    row->verified = plugin->verified ? 1u : 0u;
    put_refusal(layout, plugin, &row->refusal);
    row->capabilities_declared =
        put_strings(layout, manifest->capabilities.provides.items, manifest->capabilities.provides.count);
    row->permissions = put_strings(layout, manifest->permissions.items, manifest->permissions.count);
    row->services_provided = put_ids(layout, &plugin->services_provided);
    row->capabilities_provided = put_ids(layout, &plugin->capabilities_provided);
    row->services_consumed = put_ids(layout, &plugin->services_consumed);
}

/** Starts the writing pass: arrays from the start of `base`, texts after every array the measuring pass counted. */
static weivel_status begin_writing(weivel_layout *layout) {
    size_t arrays = layout->array_cursor;
    size_t total = arrays + layout->text_cursor;
    layout->base = weivel_memory_allocate(total == 0 ? 1 : total);
    if (layout->base == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    layout->array_cursor = 0;
    layout->text_cursor = arrays;
    return WEIVEL_STATUS_OK;
}

/** Every reported refusal of the plugins still kept, gathered and ordered by when it was reported. */
typedef struct weivel_reported {
    const weivel_reach_record **records;
    uint32_t count;
} weivel_reported;

static void merge_runs(const weivel_reach_record **from, const weivel_reach_record **into, size_t left, size_t middle,
                       size_t right) {
    size_t first = left;
    size_t second = middle;
    for (size_t index = left; index < right; index++) {
        bool take_first = first < middle && (second >= right || from[first]->sequence <= from[second]->sequence);
        into[index] = take_first ? from[first++] : from[second++];
    }
}

/** A bottom-up merge sort by sequence, since each plugin's own records arrive sorted and the core imports no qsort. */
static weivel_status sort_by_sequence(const weivel_reach_record **records, size_t count) {
    const weivel_reach_record **spare = weivel_memory_allocate(count == 0 ? 1 : count * sizeof *spare);
    if (spare == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t left = 0; left < count; left += 2 * width) {
            size_t middle = left + width < count ? left + width : count;
            size_t right = left + 2 * width < count ? left + 2 * width : count;
            merge_runs(records, spare, left, middle, right);
        }
        memcpy((void *)records, (const void *)spare, count * sizeof *records);
    }
    free((void *)spare);
    return WEIVEL_STATUS_OK;
}

static weivel_status gather_reported(const weivel_core *core, weivel_reported *reported) {
    const weivel_link *order = weivel_plugins_in_order(core->plugins);
    size_t count = 0;
    for (const weivel_link *link = order->next; link != order; link = link->next) {
        count += WEIVEL_RECORD_OF(link, weivel_plugin, in_order)->reported_count;
    }
    if (count > UINT32_MAX) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    reported->records = weivel_memory_allocate(count == 0 ? 1 : count * sizeof *reported->records);
    if (reported->records == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    reported->count = 0;
    for (const weivel_link *link = order->next; link != order; link = link->next) {
        const weivel_plugin *plugin = WEIVEL_RECORD_OF(link, weivel_plugin, in_order);
        for (uint32_t index = 0; index < plugin->reported_count; index++) {
            reported->records[reported->count++] = &plugin->reported[index];
        }
    }
    weivel_status status = sort_by_sequence(reported->records, reported->count);
    if (status != WEIVEL_STATUS_OK) {
        free((void *)reported->records);
        reported->records = NULL;
    }
    return status;
}

static void put_snapshot(weivel_layout *layout, const weivel_core *core, const weivel_reported *reported,
                         weivel_ledger_snapshot *snapshot) {
    uint32_t row_count = weivel_plugins_count(core->plugins);
    weivel_ledger_row *rows = reserve_array(layout, (size_t)row_count * sizeof *rows);
    weivel_declaration_refusal *refusals = reserve_array(layout, (size_t)reported->count * sizeof *refusals);
    const weivel_link *order = weivel_plugins_in_order(core->plugins);
    uint32_t index = 0;
    for (const weivel_link *link = order->next; link != order; link = link->next) {
        weivel_ledger_row measured;
        put_row(layout, WEIVEL_RECORD_OF(link, weivel_plugin, in_order), rows == NULL ? &measured : &rows[index++]);
    }
    for (uint32_t at = 0; at < reported->count; at++) {
        const weivel_reach_record *record = reported->records[at];
        const char *id = put_text(layout, record->id);
        if (refusals != NULL) {
            refusals[at].size = (uint32_t)sizeof refusals[at];
            refusals[at].kind = record->kind;
            refusals[at].plugin = record->plugin;
            refusals[at].id = id;
        }
    }
    snapshot->rows = row_count == 0 ? NULL : rows;
    snapshot->row_count = row_count;
    snapshot->refusals = reported->count == 0 ? NULL : refusals;
    snapshot->refusal_count = reported->count;
    snapshot->storage = layout->base;
}

static weivel_status take_snapshot(const weivel_core *core, weivel_ledger_snapshot *answer) {
    weivel_reported reported;
    weivel_status status = gather_reported(core, &reported);
    if (status != WEIVEL_STATUS_OK) {
        return status;
    }
    weivel_layout layout = {NULL, 0, 0};
    put_snapshot(&layout, core, &reported, answer);
    status = begin_writing(&layout);
    if (status == WEIVEL_STATUS_OK) {
        put_snapshot(&layout, core, &reported, answer);
    }
    free((void *)reported.records);
    return status;
}

static bool is_empty_snapshot(const weivel_ledger_snapshot *snapshot) {
    return snapshot != NULL && weivel_sized_size(snapshot) >= WEIVEL_LEDGER_SNAPSHOT_FIRST_SIZE &&
           snapshot->storage == NULL;
}

static weivel_ledger_snapshot empty_snapshot(void) {
    weivel_ledger_snapshot empty;
    memset(&empty, 0, sizeof empty);
    empty.size = (uint32_t)sizeof empty;
    return empty;
}

weivel_status weivel_ledger(weivel_core *core, weivel_ledger_snapshot *snapshot, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (!is_empty_snapshot(snapshot)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_ledger was given a snapshot missing, too small or still holding one",
            "pass a weivel_ledger_snapshot initialised with WEIVEL_INIT, or released since its last use");
    } else {
        weivel_ledger_snapshot answer = empty_snapshot();
        status = take_snapshot(core, &answer);
        if (status == WEIVEL_STATUS_OK) {
            weivel_sized_write(snapshot, &answer, sizeof answer);
        }
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

weivel_status weivel_ledger_release(weivel_ledger_snapshot *snapshot) {
    if (snapshot != NULL && snapshot->size == 0) {
        return WEIVEL_STATUS_OK;
    }
    if (snapshot == NULL || snapshot->size < WEIVEL_LEDGER_SNAPSHOT_FIRST_SIZE) {
        return WEIVEL_STATUS_MALFORMED;
    }
    free(snapshot->storage);
    weivel_ledger_snapshot empty = empty_snapshot();
    weivel_sized_write(snapshot, &empty, sizeof empty);
    return WEIVEL_STATUS_OK;
}

static weivel_status take_row(const weivel_plugin *plugin, weivel_ledger_row *answer) {
    weivel_layout layout = {NULL, 0, 0};
    put_row(&layout, plugin, answer);
    weivel_status status = begin_writing(&layout);
    if (status == WEIVEL_STATUS_OK) {
        put_row(&layout, plugin, answer);
        answer->storage = layout.base;
    }
    return status;
}

static bool is_empty_row(const weivel_ledger_row *row) {
    return row != NULL && weivel_sized_size(row) >= WEIVEL_LEDGER_ROW_FIRST_SIZE && row->storage == NULL;
}

weivel_status weivel_ledger_entry(weivel_core *core, weivel_handle plugin, weivel_ledger_row *row,
                                  uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    weivel_plugin *record = NULL;
    if (!is_empty_row(row)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_ledger_entry was given a row missing, too small or still holding one",
            "pass a weivel_ledger_row initialised with WEIVEL_INIT, or released since its last use");
    } else if ((status = weivel_core_plugin(core, plugin, &record)) == WEIVEL_STATUS_OK) {
        weivel_ledger_row answer;
        status = take_row(record, &answer);
        if (status == WEIVEL_STATUS_OK) {
            weivel_sized_write(row, &answer, sizeof answer);
        }
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

weivel_status weivel_ledger_row_release(weivel_ledger_row *row) {
    if (row != NULL && row->size == 0) {
        return WEIVEL_STATUS_OK;
    }
    if (row == NULL || row->size < WEIVEL_LEDGER_ROW_FIRST_SIZE) {
        return WEIVEL_STATUS_MALFORMED;
    }
    free(row->storage);
    weivel_ledger_row empty;
    memset(&empty, 0, sizeof empty);
    empty.size = (uint32_t)sizeof empty;
    weivel_sized_write(row, &empty, sizeof empty);
    return WEIVEL_STATUS_OK;
}
