#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "memory.h"
#include "sized.h"

#define UNVISITED UINT32_MAX

typedef struct indices {
    uint32_t *items;
    uint32_t count;
    uint32_t capacity;
} indices;

/** A plugin of the set, or the host when `manifest` is NULL. */
typedef struct unit {
    weivel_handle handle;
    const weivel_manifest *manifest;
} plan_unit;

typedef struct found_refusal {
    weivel_handle plugin;
    uint32_t code;
    char *detail;
    char *fix;
} found_refusal;

typedef struct refusals {
    found_refusal *items;
    uint32_t count;
    uint32_t capacity;
} refusal_list;

/** Tarjan's search, kept iterative so a long chain of plugins cannot exhaust the stack. */
typedef struct search {
    uint32_t *index;
    uint32_t *low;
    bool *on_stack;
    uint32_t *frame_unit;
    uint32_t *frame_position;
    indices stack;
} tarjan_search;

/**
 * Every offer and need of the set with each distinct id numbered once, so a pass reads a need's providers directly
 * instead of comparing strings against every plugin.
 *
 * - `need_start[unit]` to `need_start[unit + 1]` spans that unit's needs in `need_ids`, in manifest order.
 * - `provider_start[id]` to `provider_start[id + 1]` spans the units offering `id` in `providers`, in input order.
 */
typedef struct provider_index {
    uint32_t *need_start;
    uint32_t *need_ids;
    uint32_t *provider_start;
    uint32_t *providers;
    uint64_t *drawn_for;
    uint64_t drawing;
} provider_index;

/** Open addressing over ids the manifests own; `slots` holds an id number plus one, 0 for an empty slot. */
typedef struct id_table {
    const char **keys;
    uint32_t *slots;
    uint32_t capacity;
    uint32_t count;
} id_table;

typedef struct planner {
    uint32_t edge_source;
    const weivel_strings *host_provided;
    plan_unit *units;
    uint32_t count;
    bool *refused;
    bool *in_cycle;
    bool *unavailable;
    bool *settled;
    indices *dependencies;
    indices cycle_members;
    indices cycle_ends;
    indices order;
    refusal_list refusals;
    tarjan_search search;
    provider_index index;
} planner_state;

static bool indices_push(indices *list, uint32_t value) {
    if (list->count == list->capacity) {
        if (list->capacity > UINT32_MAX / 2) {
            return false;
        }
        uint32_t capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        uint32_t *items = weivel_memory_reallocate(list->items, (size_t)capacity * sizeof *items);
        if (items == NULL) {
            return false;
        }
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = value;
    return true;
}

static char *joined(const char *before, const char *middle, const char *after) {
    size_t lengths[3] = {strlen(before), strlen(middle), strlen(after)};
    char *text = weivel_memory_allocate(lengths[0] + lengths[1] + lengths[2] + 1);
    if (text == NULL) {
        return NULL;
    }
    memcpy(text, before, lengths[0]);
    memcpy(text + lengths[0], middle, lengths[1]);
    memcpy(text + lengths[0] + lengths[1], after, lengths[2] + 1);
    return text;
}

static bool add_refusal(refusal_list *list, weivel_handle plugin, uint32_t code, char *detail, char *fix) {
    if (detail == NULL || fix == NULL) {
        free(detail);
        free(fix);
        return false;
    }
    if (list->count == list->capacity) {
        uint32_t capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        found_refusal *items =
            list->capacity > UINT32_MAX / 2 ? NULL : weivel_memory_reallocate(list->items, (size_t)capacity * sizeof *items);
        if (items == NULL) {
            free(detail);
            free(fix);
            return false;
        }
        list->items = items;
        list->capacity = capacity;
    }
    found_refusal refusal = {plugin, code, detail, fix};
    list->items[list->count++] = refusal;
    return true;
}

static bool refuse_stale(planner_state *planner, weivel_handle handle) {
    char detail[96];
    snprintf(detail, sizeof detail, "the handle %" PRIu64 " names no plugin on this core", (uint64_t)handle);
    return add_refusal(&planner->refusals, handle, WEIVEL_REFUSAL_STALE_HANDLE, joined(detail, "", ""),
                       joined("pass only handles weivel_register_manifest answered on this core", "", ""));
}

static bool refuse_repeated(planner_state *planner, weivel_handle handle, const char *name) {
    return add_refusal(&planner->refusals, handle, WEIVEL_REFUSAL_REPEATED_PLUGIN,
                       joined("the plugin \"", name, "\" is listed more than once"),
                       joined("list each plugin once", "", ""));
}

static bool refuse_unmet_need(planner_state *planner, weivel_handle handle, const char *need) {
    return add_refusal(&planner->refusals, handle, WEIVEL_REFUSAL_UNMET_NEED,
                       joined("requires \"", need, "\", which nothing here provides"),
                       joined("install something that provides \"", need, "\", or mark the requirement optional"));
}

static bool is_host(const plan_unit *unit) {
    return unit->manifest == NULL;
}

static uint32_t offer_count(const planner_state *planner, const plan_unit *unit) {
    if (is_host(unit)) {
        return planner->edge_source == WEIVEL_EDGES_SERVICES ? planner->host_provided->count : 0;
    }
    return planner->edge_source == WEIVEL_EDGES_CAPABILITIES ? unit->manifest->capabilities.provides.count
                                                             : unit->manifest->services.provides.count;
}

static const char *offer_id(const planner_state *planner, const plan_unit *unit, uint32_t index) {
    if (is_host(unit)) {
        return planner->host_provided->items[index];
    }
    return planner->edge_source == WEIVEL_EDGES_CAPABILITIES ? unit->manifest->capabilities.provides.items[index]
                                                             : unit->manifest->services.provides.items[index].id;
}

static uint32_t need_count(const planner_state *planner, const plan_unit *unit) {
    if (is_host(unit)) {
        return 0;
    }
    return planner->edge_source == WEIVEL_EDGES_CAPABILITIES ? unit->manifest->capabilities.consumes.count
                                                             : unit->manifest->services.consumes.count;
}

static const char *need_id(const planner_state *planner, const plan_unit *unit, uint32_t index) {
    return planner->edge_source == WEIVEL_EDGES_CAPABILITIES ? unit->manifest->capabilities.consumes.items[index]
                                                             : unit->manifest->services.consumes.items[index].id;
}

/** @implNote A consumed capability is always optional, so only a service need can be mandatory. */
static bool need_is_mandatory(const planner_state *planner, const plan_unit *unit, uint32_t index) {
    return planner->edge_source == WEIVEL_EDGES_SERVICES &&
           unit->manifest->services.consumes.items[index].optional == WEIVEL_CHOICE_NO;
}

static void *zeroed(size_t count, size_t size) {
    return weivel_memory_allocate_zeroed(count == 0 ? 1 : count, size);
}

static uint64_t hash_of(const char *text) {
    uint64_t hash = 14695981039346656037u;
    for (const unsigned char *byte = (const unsigned char *)text; *byte != 0; byte++) {
        hash = (hash ^ *byte) * 1099511628211u;
    }
    return hash;
}

/** Answers the number of `key`, numbering it when it is new; `table` has room for every key it will be given. */
static uint32_t intern(id_table *table, const char *key) {
    uint32_t slot = (uint32_t)(hash_of(key) & (table->capacity - 1));
    while (table->slots[slot] != 0) {
        uint32_t id = table->slots[slot] - 1;
        if (strcmp(table->keys[id], key) == 0) {
            return id;
        }
        slot = (slot + 1) & (table->capacity - 1);
    }
    table->keys[table->count] = key;
    table->slots[slot] = ++table->count;
    return table->count - 1;
}

static size_t declaration_total(const planner_state *planner) {
    size_t total = 0;
    for (uint32_t unit = 0; unit < planner->count; unit++) {
        total += (size_t)offer_count(planner, &planner->units[unit]) + need_count(planner, &planner->units[unit]);
    }
    return total;
}

static bool open_table(id_table *table, size_t keys) {
    uint32_t capacity = 8;
    while (capacity < 2 * keys + 1) {
        capacity *= 2;
    }
    table->capacity = capacity;
    table->keys = zeroed(keys, sizeof *table->keys);
    table->slots = zeroed(capacity, sizeof *table->slots);
    return table->keys != NULL && table->slots != NULL;
}

static void number_needs(planner_state *planner, id_table *table) {
    provider_index *index = &planner->index;
    uint32_t written = 0;
    for (uint32_t unit = 0; unit < planner->count; unit++) {
        index->need_start[unit] = written;
        uint32_t needs = need_count(planner, &planner->units[unit]);
        for (uint32_t need = 0; need < needs; need++) {
            index->need_ids[written++] = intern(table, need_id(planner, &planner->units[unit], need));
        }
    }
    index->need_start[planner->count] = written;
}

static void count_providers(planner_state *planner, id_table *table, uint32_t *offer_ids, uint32_t *last_unit) {
    uint32_t *provider_start = planner->index.provider_start;
    uint32_t written = 0;
    for (uint32_t unit = 0; unit < planner->count; unit++) {
        uint32_t count = offer_count(planner, &planner->units[unit]);
        for (uint32_t offer = 0; offer < count; offer++) {
            uint32_t id = intern(table, offer_id(planner, &planner->units[unit], offer));
            offer_ids[written++] = id;
            if (last_unit[id] != unit + 1) {
                last_unit[id] = unit + 1;
                provider_start[id + 1]++;
            }
        }
    }
    for (uint32_t id = 0; id < table->count; id++) {
        provider_start[id + 1] += provider_start[id];
        last_unit[id] = 0;
    }
}

static void place_providers(planner_state *planner, const id_table *table, const uint32_t *offer_ids,
                            uint32_t *last_unit) {
    provider_index *index = &planner->index;
    uint32_t read = 0;
    for (uint32_t unit = 0; unit < planner->count; unit++) {
        uint32_t count = offer_count(planner, &planner->units[unit]);
        for (uint32_t offer = 0; offer < count; offer++) {
            uint32_t id = offer_ids[read++];
            if (last_unit[id] != unit + 1) {
                last_unit[id] = unit + 1;
                index->providers[index->provider_start[id]++] = unit;
            }
        }
    }
    for (uint32_t id = table->count; id > 0; id--) {
        index->provider_start[id] = index->provider_start[id - 1];
    }
    index->provider_start[0] = 0;
}

/** Lays each id's providers out contiguously in input order, a unit listed once per id however often it offers it. */
static bool number_offers(planner_state *planner, id_table *table, size_t offers) {
    provider_index *index = &planner->index;
    uint32_t *offer_ids = zeroed(offers, sizeof *offer_ids);
    uint32_t *last_unit = zeroed(table->capacity, sizeof *last_unit);
    index->provider_start = zeroed((size_t)table->capacity + 1, sizeof *index->provider_start);
    index->providers = zeroed(offers, sizeof *index->providers);
    bool built = offer_ids != NULL && last_unit != NULL && index->provider_start != NULL && index->providers != NULL;
    if (built) {
        count_providers(planner, table, offer_ids, last_unit);
        place_providers(planner, table, offer_ids, last_unit);
    }
    free(offer_ids);
    free(last_unit);
    return built;
}

static bool index_providers(planner_state *planner) {
    size_t total = declaration_total(planner);
    if (total > UINT32_MAX / 4) {
        return false;
    }
    provider_index *index = &planner->index;
    index->need_start = zeroed((size_t)planner->count + 1, sizeof *index->need_start);
    index->need_ids = zeroed(total, sizeof *index->need_ids);
    index->drawn_for = zeroed(planner->count, sizeof *index->drawn_for);
    id_table table;
    memset(&table, 0, sizeof table);
    bool built = index->need_start != NULL && index->need_ids != NULL && index->drawn_for != NULL &&
                 open_table(&table, total);
    if (built) {
        number_needs(planner, &table);
        built = number_offers(planner, &table, total);
    }
    free(table.keys);
    free(table.slots);
    return built;
}

static bool draw_dependencies_of(planner_state *planner, uint32_t consumer) {
    provider_index *index = &planner->index;
    indices *dependencies = &planner->dependencies[consumer];
    uint64_t drawing = ++index->drawing;
    for (uint32_t need = index->need_start[consumer]; need < index->need_start[consumer + 1]; need++) {
        uint32_t id = index->need_ids[need];
        for (uint32_t entry = index->provider_start[id]; entry < index->provider_start[id + 1]; entry++) {
            uint32_t provider = index->providers[entry];
            if (provider == consumer || planner->refused[provider] || index->drawn_for[provider] == drawing) {
                continue;
            }
            index->drawn_for[provider] = drawing;
            if (!indices_push(dependencies, provider)) {
                return false;
            }
        }
    }
    return true;
}

static bool draw_dependencies(planner_state *planner) {
    for (uint32_t consumer = 0; consumer < planner->count; consumer++) {
        planner->dependencies[consumer].count = 0;
        if (!planner->refused[consumer] && !draw_dependencies_of(planner, consumer)) {
            return false;
        }
    }
    return true;
}

static bool enter(planner_state *planner, uint32_t unit, uint32_t *next_index, uint32_t *depth) {
    tarjan_search *search = &planner->search;
    search->index[unit] = *next_index;
    search->low[unit] = *next_index;
    (*next_index)++;
    search->on_stack[unit] = true;
    search->frame_unit[*depth] = unit;
    search->frame_position[*depth] = 0;
    (*depth)++;
    return indices_push(&search->stack, unit);
}

static uint32_t smaller(uint32_t left, uint32_t right) {
    return left < right ? left : right;
}

/** Pops the component `root` closes; one of two or more members is a cycle, recorded dependency first. */
static bool close_component(planner_state *planner, uint32_t root) {
    indices *stack = &planner->search.stack;
    uint32_t start = stack->count;
    do {
        start--;
    } while (stack->items[start] != root);
    uint32_t size = stack->count - start;
    for (uint32_t position = start; position < stack->count; position++) {
        uint32_t member = stack->items[position];
        planner->search.on_stack[member] = false;
        if (size > 1) {
            planner->in_cycle[member] = true;
            if (!indices_push(&planner->cycle_members, member)) {
                return false;
            }
        }
    }
    stack->count = start;
    return size == 1 || indices_push(&planner->cycle_ends, planner->cycle_members.count);
}

static bool search_from(planner_state *planner, uint32_t root, uint32_t *next_index) {
    tarjan_search *search = &planner->search;
    uint32_t depth = 0;
    if (!enter(planner, root, next_index, &depth)) {
        return false;
    }
    while (depth > 0) {
        uint32_t unit = search->frame_unit[depth - 1];
        const indices *dependencies = &planner->dependencies[unit];
        if (search->frame_position[depth - 1] < dependencies->count) {
            uint32_t dependency = dependencies->items[search->frame_position[depth - 1]++];
            if (search->index[dependency] == UNVISITED) {
                if (!enter(planner, dependency, next_index, &depth)) {
                    return false;
                }
            } else if (search->on_stack[dependency]) {
                search->low[unit] = smaller(search->low[unit], search->index[dependency]);
            }
            continue;
        }
        depth--;
        if (search->low[unit] == search->index[unit] && !close_component(planner, unit)) {
            return false;
        }
        if (depth > 0) {
            uint32_t parent = search->frame_unit[depth - 1];
            search->low[parent] = smaller(search->low[parent], search->low[unit]);
        }
    }
    return true;
}

static bool find_cycles(planner_state *planner) {
    planner->cycle_members.count = 0;
    planner->cycle_ends.count = 0;
    planner->search.stack.count = 0;
    for (uint32_t unit = 0; unit < planner->count; unit++) {
        planner->in_cycle[unit] = false;
        planner->search.index[unit] = UNVISITED;
        planner->search.on_stack[unit] = false;
    }
    uint32_t next_index = 0;
    for (uint32_t unit = 0; unit < planner->count; unit++) {
        if (!planner->refused[unit] && planner->search.index[unit] == UNVISITED &&
            !search_from(planner, unit, &next_index)) {
            return false;
        }
    }
    return true;
}

static bool is_met(const planner_state *planner, uint32_t id) {
    const provider_index *index = &planner->index;
    for (uint32_t entry = index->provider_start[id]; entry < index->provider_start[id + 1]; entry++) {
        if (!planner->unavailable[index->providers[entry]]) {
            return true;
        }
    }
    return false;
}

static const char *unmet_need(const planner_state *planner, uint32_t consumer) {
    const plan_unit *unit = &planner->units[consumer];
    uint32_t first = planner->index.need_start[consumer];
    uint32_t needs = need_count(planner, unit);
    for (uint32_t need = 0; need < needs; need++) {
        if (need_is_mandatory(planner, unit, need) && !is_met(planner, planner->index.need_ids[first + need])) {
            return need_id(planner, unit, need);
        }
    }
    return NULL;
}

/**
 * Refuses each plugin whose mandatory need nothing available meets, judged against who was available when the
 * pass began. Answers 1 when it refused another, 0 when not, -1 when memory ran out.
 */
static int refuse_unmet(planner_state *planner) {
    for (uint32_t unit = 0; unit < planner->count; unit++) {
        planner->unavailable[unit] = planner->refused[unit] || planner->in_cycle[unit];
    }
    int refused_more = 0;
    for (uint32_t index = 0; index < planner->count; index++) {
        if (planner->unavailable[index]) {
            continue;
        }
        const char *need = unmet_need(planner, index);
        if (need == NULL) {
            continue;
        }
        planner->refused[index] = true;
        refused_more = 1;
        if (!refuse_unmet_need(planner, planner->units[index].handle, need)) {
            return -1;
        }
    }
    return refused_more;
}

static bool providers_settled(const planner_state *planner, uint32_t consumer) {
    const indices *dependencies = &planner->dependencies[consumer];
    for (uint32_t index = 0; index < dependencies->count; index++) {
        if (!planner->settled[dependencies->items[index]]) {
            return false;
        }
    }
    return true;
}

static bool sequence(planner_state *planner) {
    for (uint32_t unit = 0; unit < planner->count; unit++) {
        planner->settled[unit] = planner->in_cycle[unit];
    }
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (uint32_t unit = 0; unit < planner->count; unit++) {
            if (planner->refused[unit] || planner->settled[unit] || !providers_settled(planner, unit)) {
                continue;
            }
            planner->settled[unit] = true;
            progressed = true;
            if (!is_host(&planner->units[unit]) && !indices_push(&planner->order, unit)) {
                return false;
            }
        }
    }
    return true;
}

static weivel_status run_passes(planner_state *planner) {
    int refused_more;
    do {
        if (!draw_dependencies(planner) || !find_cycles(planner)) {
            return WEIVEL_STATUS_OUT_OF_MEMORY;
        }
        refused_more = refuse_unmet(planner);
    } while (refused_more == 1);
    if (refused_more < 0 || !sequence(planner)) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    return WEIVEL_STATUS_OK;
}

static bool listed_before(const planner_state *planner, weivel_handle handle) {
    for (uint32_t index = 0; index < planner->count; index++) {
        if (planner->units[index].handle == handle) {
            return true;
        }
    }
    return false;
}

static bool admit(planner_state *planner, const weivel_handles *handles, weivel_handle handle) {
    void *record = NULL;
    if (weivel_handles_resolve(handles, handle, WEIVEL_RECORD_PLUGIN, &record) != WEIVEL_STATUS_OK) {
        return refuse_stale(planner, handle);
    }
    const weivel_plugin *plugin = record;
    if (listed_before(planner, handle)) {
        return refuse_repeated(planner, handle, plugin->manifest.value.import_name);
    }
    plan_unit admitted = {handle, &plugin->manifest.value};
    planner->units[planner->count++] = admitted;
    return true;
}

static bool admit_all(planner_state *planner, const weivel_handles *handles, const weivel_handle *plugins,
                      uint32_t count) {
    for (uint32_t index = 0; index < count; index++) {
        if (!admit(planner, handles, plugins[index])) {
            return false;
        }
    }
    if (planner->host_provided->count > 0) {
        plan_unit host = {0, NULL};
        planner->units[planner->count++] = host;
    }
    return true;
}

static bool allocate(planner_state *planner, uint32_t capacity) {
    planner->units = zeroed(capacity, sizeof *planner->units);
    planner->refused = zeroed(capacity, sizeof *planner->refused);
    planner->in_cycle = zeroed(capacity, sizeof *planner->in_cycle);
    planner->unavailable = zeroed(capacity, sizeof *planner->unavailable);
    planner->settled = zeroed(capacity, sizeof *planner->settled);
    planner->dependencies = zeroed(capacity, sizeof *planner->dependencies);
    planner->search.index = zeroed(capacity, sizeof *planner->search.index);
    planner->search.low = zeroed(capacity, sizeof *planner->search.low);
    planner->search.on_stack = zeroed(capacity, sizeof *planner->search.on_stack);
    planner->search.frame_unit = zeroed(capacity, sizeof *planner->search.frame_unit);
    planner->search.frame_position = zeroed(capacity, sizeof *planner->search.frame_position);
    return planner->units != NULL && planner->refused != NULL && planner->in_cycle != NULL &&
           planner->unavailable != NULL && planner->settled != NULL && planner->dependencies != NULL &&
           planner->search.index != NULL && planner->search.low != NULL && planner->search.on_stack != NULL &&
           planner->search.frame_unit != NULL && planner->search.frame_position != NULL;
}

static void free_planner(planner_state *planner, uint32_t capacity) {
    if (planner->dependencies != NULL) {
        for (uint32_t index = 0; index < capacity; index++) {
            free(planner->dependencies[index].items);
        }
    }
    for (uint32_t index = 0; index < planner->refusals.count; index++) {
        free(planner->refusals.items[index].detail);
        free(planner->refusals.items[index].fix);
    }
    free(planner->refusals.items);
    free(planner->units);
    free(planner->refused);
    free(planner->in_cycle);
    free(planner->unavailable);
    free(planner->settled);
    free(planner->dependencies);
    free(planner->cycle_members.items);
    free(planner->cycle_ends.items);
    free(planner->order.items);
    free(planner->search.index);
    free(planner->search.low);
    free(planner->search.on_stack);
    free(planner->search.frame_unit);
    free(planner->search.frame_position);
    free(planner->search.stack.items);
    free(planner->index.need_start);
    free(planner->index.need_ids);
    free(planner->index.provider_start);
    free(planner->index.providers);
    free(planner->index.drawn_for);
}

/** The answer's one allocation, laid out in sections each rounded to 8 bytes, which aligns every element type. */
typedef struct layout {
    size_t refusals;
    size_t cycles;
    size_t edges;
    size_t order;
    size_t members;
    size_t texts;
    size_t total;
} answer_layout;

static size_t rounded(size_t size) {
    return (size + 7) & ~(size_t)7;
}

static uint32_t edge_count(const planner_state *planner) {
    uint32_t count = 0;
    for (uint32_t consumer = 0; consumer < planner->count; consumer++) {
        const indices *dependencies = &planner->dependencies[consumer];
        for (uint32_t index = 0; index < dependencies->count; index++) {
            count += is_host(&planner->units[dependencies->items[index]]) ? 0 : 1;
        }
    }
    return count;
}

static answer_layout lay_out(const planner_state *planner, uint32_t edges) {
    answer_layout layout;
    size_t texts = 0;
    for (uint32_t index = 0; index < planner->refusals.count; index++) {
        texts += strlen(planner->refusals.items[index].detail) + strlen(planner->refusals.items[index].fix) + 2;
    }
    layout.refusals = 0;
    layout.cycles = layout.refusals + rounded((size_t)planner->refusals.count * sizeof(weivel_plugin_refusal));
    layout.edges = layout.cycles + rounded((size_t)planner->cycle_ends.count * sizeof(weivel_cycle));
    layout.order = layout.edges + rounded((size_t)edges * sizeof(weivel_edge));
    layout.members = layout.order + rounded((size_t)planner->order.count * sizeof(weivel_handle));
    layout.texts = layout.members + rounded((size_t)planner->cycle_members.count * sizeof(weivel_handle));
    layout.total = layout.texts + texts;
    return layout;
}

static const char *copy_text(char **cursor, const char *text) {
    size_t length = strlen(text) + 1;
    memcpy(*cursor, text, length);
    const char *copy = *cursor;
    *cursor += length;
    return copy;
}

static void fill_refusals(const planner_state *planner, weivel_plugin_refusal *refusals, char *texts) {
    for (uint32_t index = 0; index < planner->refusals.count; index++) {
        const found_refusal *found = &planner->refusals.items[index];
        refusals[index].plugin = found->plugin;
        refusals[index].refusal.code = found->code;
        refusals[index].refusal.detail = copy_text(&texts, found->detail);
        refusals[index].refusal.fix = copy_text(&texts, found->fix);
    }
}

static void fill_cycles(const planner_state *planner, weivel_cycle *cycles, weivel_handle *members) {
    uint32_t start = 0;
    for (uint32_t cycle = 0; cycle < planner->cycle_ends.count; cycle++) {
        uint32_t end = planner->cycle_ends.items[cycle];
        cycles[cycle].members = members + start;
        cycles[cycle].count = end - start;
        for (uint32_t position = start; position < end; position++) {
            members[position] = planner->units[planner->cycle_members.items[position]].handle;
        }
        start = end;
    }
}

static void fill_edges(const planner_state *planner, weivel_edge *edges) {
    uint32_t written = 0;
    for (uint32_t consumer = 0; consumer < planner->count; consumer++) {
        const indices *dependencies = &planner->dependencies[consumer];
        for (uint32_t index = 0; index < dependencies->count; index++) {
            const plan_unit *provider = &planner->units[dependencies->items[index]];
            if (!is_host(provider)) {
                edges[written].provider = provider->handle;
                edges[written].consumer = planner->units[consumer].handle;
                written++;
            }
        }
    }
}

static void *section(char *storage, size_t offset, uint32_t count) {
    return count == 0 ? NULL : storage + offset;
}

static weivel_status answer_plan(const planner_state *planner, weivel_activation_plan *answer) {
    uint32_t edges = edge_count(planner);
    answer_layout layout = lay_out(planner, edges);
    char *storage = weivel_memory_allocate(layout.total == 0 ? 1 : layout.total);
    if (storage == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    weivel_plugin_refusal *refusals = section(storage, layout.refusals, planner->refusals.count);
    weivel_cycle *cycles = section(storage, layout.cycles, planner->cycle_ends.count);
    weivel_edge *edge_items = section(storage, layout.edges, edges);
    weivel_handle *order = section(storage, layout.order, planner->order.count);
    weivel_handle *members = section(storage, layout.members, planner->cycle_members.count);
    fill_refusals(planner, refusals, storage + layout.texts);
    fill_cycles(planner, cycles, members);
    fill_edges(planner, edge_items);
    for (uint32_t index = 0; index < planner->order.count; index++) {
        order[index] = planner->units[planner->order.items[index]].handle;
    }
    answer->storage = storage;
    answer->refusals = refusals;
    answer->refusal_count = planner->refusals.count;
    answer->cycles = cycles;
    answer->cycle_count = planner->cycle_ends.count;
    answer->edges = edge_items;
    answer->edge_count = edges;
    answer->order = order;
    answer->order_count = planner->order.count;
    return WEIVEL_STATUS_OK;
}

static weivel_activation_plan empty_plan(void) {
    weivel_activation_plan plan;
    memset(&plan, 0, sizeof plan);
    plan.size = (uint32_t)sizeof plan;
    return plan;
}

static weivel_status plan_activation(weivel_core *core, const weivel_handle *plugins, uint32_t count,
                                     uint32_t edge_source, const weivel_strings *host_provided,
                                     weivel_activation_plan *callers) {
    planner_state planner;
    memset(&planner, 0, sizeof planner);
    planner.edge_source = edge_source;
    planner.host_provided = host_provided;
    if (count == UINT32_MAX) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    uint32_t capacity = count + 1;
    weivel_status status = WEIVEL_STATUS_OUT_OF_MEMORY;
    weivel_activation_plan answer = empty_plan();
    if (allocate(&planner, capacity) && admit_all(&planner, core->handles, plugins, count) &&
        index_providers(&planner)) {
        status = run_passes(&planner);
    }
    if (status == WEIVEL_STATUS_OK) {
        status = answer_plan(&planner, &answer);
    }
    free_planner(&planner, capacity);
    if (status == WEIVEL_STATUS_OK) {
        weivel_sized_write(callers, &answer, sizeof answer);
    }
    return status;
}

static bool is_sized_plan(const weivel_activation_plan *plan) {
    return plan != NULL && plan->size >= WEIVEL_ACTIVATION_PLAN_FIRST_SIZE;
}

static bool is_edge_source(uint32_t edge_source) {
    return edge_source == WEIVEL_EDGES_CAPABILITIES || edge_source == WEIVEL_EDGES_SERVICES;
}

static weivel_status check_and_plan(weivel_core *core, const weivel_handle *plugins, uint32_t count,
                                    uint32_t edge_source, const weivel_strings *host_provided,
                                    weivel_activation_plan *plan) {
    static const weivel_strings no_host_services = {NULL, 0};
    if (!is_sized_plan(plan)) {
        return weivel_core_refuse_malformed(
            core, "weivel_plan_activation was given no plan struct, or one whose size is too small",
            "pass a weivel_activation_plan initialised with WEIVEL_INIT(weivel_activation_plan)");
    }
    if (plan->storage != NULL) {
        return weivel_core_refuse_malformed(core, "weivel_plan_activation was given a plan still holding one",
                                            "pass the held plan to weivel_activation_plan_release first");
    }
    if (plugins == NULL && count > 0) {
        return weivel_core_refuse_malformed(core, "weivel_plan_activation was given a count of plugins but none",
                                            "point plugins at the handles, or pass a count of 0");
    }
    if (!is_edge_source(edge_source)) {
        return weivel_core_refuse_malformed(
            core, "weivel_plan_activation was given an edge source that is no weivel_edge_source",
            "pass WEIVEL_EDGES_CAPABILITIES or WEIVEL_EDGES_SERVICES");
    }
    if (host_provided != NULL && !weivel_strings_readable(host_provided)) {
        return weivel_core_refuse_malformed(
            core, "weivel_plan_activation was given host services that count entries it does not give, or a NULL",
            "give every host service id, or pass NULL for none");
    }
    return plan_activation(core, plugins, count, edge_source, host_provided == NULL ? &no_host_services : host_provided,
                           plan);
}

weivel_status weivel_plan_activation(weivel_core *core, const weivel_handle *plugins, uint32_t count,
                                     uint32_t edge_source, const weivel_strings *host_provided,
                                     weivel_activation_plan *plan, uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status = check_and_plan(core, plugins, count, edge_source, host_provided, plan);
    weivel_core_end_call(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

weivel_status weivel_activation_plan_release(weivel_activation_plan *plan) {
    if (plan != NULL && plan->size == 0) {
        return WEIVEL_STATUS_OK;
    }
    if (!is_sized_plan(plan)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    free(plan->storage);
    weivel_activation_plan empty = empty_plan();
    weivel_sized_write(plan, &empty, sizeof empty);
    return WEIVEL_STATUS_OK;
}
