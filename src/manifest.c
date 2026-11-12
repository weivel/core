#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sized.h"
#include "text.h"

typedef struct element_list {
    const unsigned char *items;
    uint32_t count;
    uint32_t stride;
} element_list;

typedef struct shape_check {
    const char *plugin;
    weivel_manifest_problem *problem;
} shape_check;

void weivel_manifest_problem_free(weivel_manifest_problem *problem) {
    free(problem->detail);
    free(problem->fix);
    problem->detail = NULL;
    problem->fix = NULL;
}

/** Fills the problem from `detail` and `fix`, each built by weivel_text_format or NULL, and frees both. */
static bool refuse_shape(shape_check *check, char *detail, char *fix) {
    check->problem->detail = weivel_text_format("the manifest of \"%s\" %s", check->plugin,
                                                detail == NULL ? "is malformed" : detail);
    check->problem->fix = fix;
    free(detail);
    return false;
}

static bool refuse_null_items(shape_check *check, const char *field, uint32_t count) {
    return refuse_shape(check,
                        weivel_text_format("declares %u %s in %s but gives none", (unsigned)count,
                                           count == 1 ? "entry" : "entries", field),
                        weivel_text_format("list the entries of %s, or declare none", field));
}

static bool refuse_missing(shape_check *check, const char *field, uint32_t index, const char *what,
                           const char *one) {
    return refuse_shape(
        check, weivel_text_format("has an entry with no %s in %s (entry %u)", what, field, (unsigned)index + 1),
        weivel_text_format("give entry %u of %s %s %s, or remove it", (unsigned)index + 1, field, one, what));
}

static bool refuse_twice(shape_check *check, const char *field, const char *value) {
    return refuse_shape(check, weivel_text_format("lists \"%s\" twice in %s", value, field),
                        weivel_text_format("list \"%s\" once in %s", value, field));
}

static bool is_empty(const char *text) {
    return text == NULL || text[0] == '\0';
}

static bool appears_before(const char *const *texts, uint32_t index) {
    for (uint32_t earlier = 0; earlier < index; earlier++) {
        if (strcmp(texts[earlier], texts[index]) == 0) {
            return true;
        }
    }
    return false;
}

bool weivel_strings_readable(const weivel_strings *strings) {
    if (strings->count > 0 && strings->items == NULL) {
        return false;
    }
    for (uint32_t index = 0; index < strings->count; index++) {
        if (strings->items[index] == NULL) {
            return false;
        }
    }
    return true;
}

static bool check_strings(shape_check *check, const weivel_strings *strings, const char *field) {
    if (strings->count > 0 && strings->items == NULL) {
        return refuse_null_items(check, field, strings->count);
    }
    for (uint32_t index = 0; index < strings->count; index++) {
        if (is_empty(strings->items[index])) {
            return refuse_missing(check, field, index, "value", "a");
        }
        if (appears_before(strings->items, index)) {
            return refuse_twice(check, field, strings->items[index]);
        }
    }
    return true;
}

static const void *element_at(const element_list *list, uint32_t index) {
    return list->items + (size_t)index * list->stride;
}

static element_list list_of(const void *items, uint32_t count) {
    element_list list = {items, count, 0};
    if (count > 0 && items != NULL) {
        list.stride = weivel_sized_size(items);
    }
    return list;
}

static bool check_elements(shape_check *check, const element_list *list, size_t first_size, const char *field) {
    if (list->count == 0) {
        return true;
    }
    if (list->items == NULL) {
        return refuse_null_items(check, field, list->count);
    }
    if (list->stride < first_size) {
        return refuse_shape(check,
                            weivel_text_format("gives %s elements of size %u, smaller than their first shape of %u",
                                               field, (unsigned)list->stride, (unsigned)first_size),
                            weivel_text_format("initialise each element with WEIVEL_INIT"));
    }
    for (uint32_t index = 1; index < list->count; index++) {
        uint32_t size = weivel_sized_size(element_at(list, index));
        if (size != list->stride) {
            return refuse_shape(check,
                                weivel_text_format("gives entry %u of %s size %u where its first entry gives %u",
                                                   (unsigned)index + 1, field, (unsigned)size,
                                                   (unsigned)list->stride),
                                weivel_text_format("build each list as one array of one element type"));
        }
    }
    return true;
}

static weivel_number number_at(const element_list *list, uint32_t index) {
    weivel_number number;
    weivel_sized_read(&number, sizeof number, element_at(list, index));
    return number;
}

static weivel_service_offer offer_at(const element_list *list, uint32_t index) {
    weivel_service_offer offer;
    weivel_sized_read(&offer, sizeof offer, element_at(list, index));
    return offer;
}

static weivel_service_want want_at(const element_list *list, uint32_t index) {
    weivel_service_want want;
    weivel_sized_read(&want, sizeof want, element_at(list, index));
    return want;
}

static bool number_named_before(const element_list *list, uint32_t index, const char *name) {
    for (uint32_t earlier = 0; earlier < index; earlier++) {
        if (strcmp(number_at(list, earlier).name, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool check_numbers(shape_check *check, const weivel_numbers *numbers, const char *field) {
    element_list list = list_of(numbers->items, numbers->count);
    if (!check_elements(check, &list, WEIVEL_NUMBER_FIRST_SIZE, field)) {
        return false;
    }
    for (uint32_t index = 0; index < list.count; index++) {
        weivel_number number = number_at(&list, index);
        if (is_empty(number.name)) {
            return refuse_missing(check, field, index, "name", "a");
        }
        if (number_named_before(&list, index, number.name)) {
            return refuse_twice(check, field, number.name);
        }
    }
    return true;
}

/** Checks a service's numbers under a field named for the service, built at its length. */
static bool check_named_numbers(shape_check *check, const weivel_numbers *numbers, const char *what,
                                const char *service) {
    if (numbers->count == 0) {
        return true;
    }
    char *field = weivel_text_format("%s \"%s\"", what, service);
    bool passed = check_numbers(check, numbers, field == NULL ? what : field);
    free(field);
    return passed;
}

static bool offer_named_before(const element_list *list, uint32_t index, const char *id) {
    for (uint32_t earlier = 0; earlier < index; earlier++) {
        if (strcmp(offer_at(list, earlier).id, id) == 0) {
            return true;
        }
    }
    return false;
}

static bool want_named_before(const element_list *list, uint32_t index, const char *id) {
    for (uint32_t earlier = 0; earlier < index; earlier++) {
        if (strcmp(want_at(list, earlier).id, id) == 0) {
            return true;
        }
    }
    return false;
}

static bool check_offers(shape_check *check, const weivel_service_offers *offers) {
    const char *field = "the services it provides";
    element_list list = list_of(offers->items, offers->count);
    if (!check_elements(check, &list, WEIVEL_SERVICE_OFFER_FIRST_SIZE, field)) {
        return false;
    }
    for (uint32_t index = 0; index < list.count; index++) {
        weivel_service_offer offer = offer_at(&list, index);
        if (is_empty(offer.id)) {
            return refuse_missing(check, field, index, "id", "an");
        }
        if (offer_named_before(&list, index, offer.id)) {
            return refuse_twice(check, field, offer.id);
        }
        if (!check_named_numbers(check, &offer.attributes, "the attributes of the service", offer.id)) {
            return false;
        }
    }
    return true;
}

static bool check_choice(shape_check *check, uint32_t choice, const char *service, const char *name) {
    if (choice <= WEIVEL_CHOICE_YES) {
        return true;
    }
    return refuse_shape(
        check,
        weivel_text_format("says whether the service \"%s\" is %s with %u, which is neither yes, no nor absent",
                           service, name, (unsigned)choice),
        weivel_text_format("set it to WEIVEL_CHOICE_DEFAULT, WEIVEL_CHOICE_NO or WEIVEL_CHOICE_YES"));
}

static bool check_wants(shape_check *check, const weivel_service_wants *wants) {
    const char *field = "the services it consumes";
    element_list list = list_of(wants->items, wants->count);
    if (!check_elements(check, &list, WEIVEL_SERVICE_WANT_FIRST_SIZE, field)) {
        return false;
    }
    for (uint32_t index = 0; index < list.count; index++) {
        weivel_service_want want = want_at(&list, index);
        if (is_empty(want.id)) {
            return refuse_missing(check, field, index, "id", "an");
        }
        if (want_named_before(&list, index, want.id)) {
            return refuse_twice(check, field, want.id);
        }
        if (!check_choice(check, want.optional, want.id, "optional") ||
            !check_choice(check, want.degradable, want.id, "degradable") ||
            !check_named_numbers(check, &want.constraints, "the constraints on the service", want.id)) {
            return false;
        }
    }
    return true;
}

static bool check_shape(const weivel_manifest *manifest, weivel_manifest_problem *problem) {
    shape_check check = {manifest->import_name, problem};
    if (is_empty(manifest->import_name)) {
        problem->detail = weivel_text_format("the manifest has no import name");
        problem->fix = weivel_text_format("give the manifest an import name");
        return false;
    }
    if (manifest->identity != NULL && manifest->identity[0] == '\0') {
        return refuse_shape(&check, weivel_text_format("gives an empty identity"),
                            weivel_text_format("give it an identity, or none for its import name"));
    }
    return check_strings(&check, &manifest->targets, "targets") &&
           check_strings(&check, &manifest->capabilities.provides, "the capabilities it provides") &&
           check_strings(&check, &manifest->capabilities.consumes, "the capabilities it consumes") &&
           check_offers(&check, &manifest->services.provides) && check_wants(&check, &manifest->services.consumes) &&
           check_strings(&check, &manifest->permissions, "permissions");
}

/** Lays out one allocation; with no `base` it only measures, so the same walk sizes the block and then fills it. */
typedef struct arena {
    unsigned char *base;
    size_t used;
} arena;

typedef union widest_field {
    double number;
    void *pointer;
    uint64_t integer;
} widest_field;

static void *take(arena *block, size_t bytes) {
    size_t alignment = sizeof(widest_field);
    block->used = (block->used + alignment - 1) / alignment * alignment;
    void *taken = block->base == NULL ? NULL : block->base + block->used;
    block->used += bytes;
    return taken;
}

static const char *keep_text(arena *block, const char *text) {
    if (text == NULL) {
        return NULL;
    }
    size_t length = strlen(text) + 1;
    char *kept = take(block, length);
    if (kept != NULL) {
        memcpy(kept, text, length);
    }
    return kept;
}

static weivel_strings keep_strings(arena *block, const weivel_strings *source) {
    weivel_strings kept = {NULL, source->count};
    if (source->count == 0) {
        return kept;
    }
    const char **items = take(block, (size_t)source->count * sizeof *items);
    for (uint32_t index = 0; index < source->count; index++) {
        const char *text = keep_text(block, source->items[index]);
        if (items != NULL) {
            items[index] = text;
        }
    }
    kept.items = items;
    return kept;
}

static weivel_numbers keep_numbers(arena *block, const weivel_numbers *source) {
    weivel_numbers kept = {NULL, source->count};
    if (source->count == 0) {
        return kept;
    }
    element_list list = list_of(source->items, source->count);
    weivel_number *items = take(block, (size_t)source->count * sizeof *items);
    for (uint32_t index = 0; index < source->count; index++) {
        weivel_number number = number_at(&list, index);
        number.size = (uint32_t)sizeof number;
        number.name = keep_text(block, number.name);
        if (items != NULL) {
            items[index] = number;
        }
    }
    kept.items = items;
    return kept;
}

static weivel_service_offers keep_offers(arena *block, const weivel_service_offers *source) {
    weivel_service_offers kept = {NULL, source->count};
    if (source->count == 0) {
        return kept;
    }
    element_list list = list_of(source->items, source->count);
    weivel_service_offer *items = take(block, (size_t)source->count * sizeof *items);
    for (uint32_t index = 0; index < source->count; index++) {
        weivel_service_offer offer = offer_at(&list, index);
        offer.size = (uint32_t)sizeof offer;
        offer.id = keep_text(block, offer.id);
        offer.attributes = keep_numbers(block, &offer.attributes);
        if (items != NULL) {
            items[index] = offer;
        }
    }
    kept.items = items;
    return kept;
}

static uint32_t resolved(uint32_t choice, uint32_t default_choice) {
    return choice == WEIVEL_CHOICE_DEFAULT ? default_choice : choice;
}

static weivel_service_wants keep_wants(arena *block, const weivel_service_wants *source) {
    weivel_service_wants kept = {NULL, source->count};
    if (source->count == 0) {
        return kept;
    }
    element_list list = list_of(source->items, source->count);
    weivel_service_want *items = take(block, (size_t)source->count * sizeof *items);
    for (uint32_t index = 0; index < source->count; index++) {
        weivel_service_want want = want_at(&list, index);
        want.size = (uint32_t)sizeof want;
        want.id = keep_text(block, want.id);
        want.provider = keep_text(block, want.provider);
        want.optional = resolved(want.optional, WEIVEL_CHOICE_YES);
        want.degradable = resolved(want.degradable, WEIVEL_CHOICE_NO);
        want.constraints = keep_numbers(block, &want.constraints);
        if (items != NULL) {
            items[index] = want;
        }
    }
    kept.items = items;
    return kept;
}

static weivel_manifest keep_manifest(arena *block, const weivel_manifest *source) {
    weivel_manifest kept = *source;
    kept.size = (uint32_t)sizeof kept;
    kept.import_name = keep_text(block, source->import_name);
    kept.entry = keep_text(block, source->entry);
    kept.targets = keep_strings(block, &source->targets);
    kept.capabilities.provides = keep_strings(block, &source->capabilities.provides);
    kept.capabilities.consumes = keep_strings(block, &source->capabilities.consumes);
    kept.services.provides = keep_offers(block, &source->services.provides);
    kept.services.consumes = keep_wants(block, &source->services.consumes);
    kept.permissions = keep_strings(block, &source->permissions);
    kept.identity = source->identity == NULL ? kept.import_name : keep_text(block, source->identity);
    return kept;
}

weivel_status weivel_manifest_copy(const weivel_manifest *callers, weivel_owned_manifest *owned,
                                   weivel_manifest_problem *problem) {
    weivel_manifest view;
    weivel_sized_read(&view, sizeof view, callers);
    memset(owned, 0, sizeof *owned);
    problem->detail = NULL;
    problem->fix = NULL;
    if (!check_shape(&view, problem)) {
        return WEIVEL_STATUS_REFUSED;
    }
    arena measuring = {NULL, 0};
    keep_manifest(&measuring, &view);
    arena filling = {calloc(1, measuring.used == 0 ? 1 : measuring.used), 0};
    if (filling.base == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    owned->value = keep_manifest(&filling, &view);
    owned->storage = filling.base;
    return WEIVEL_STATUS_OK;
}

void weivel_manifest_free(weivel_owned_manifest *owned) {
    free(owned->storage);
    memset(owned, 0, sizeof *owned);
}

bool weivel_elements_readable(const void *items, uint32_t count, size_t first_size) {
    element_list list = list_of(items, count);
    if (count == 0) {
        return true;
    }
    if (items == NULL || list.stride < first_size) {
        return false;
    }
    for (uint32_t index = 1; index < count; index++) {
        if (weivel_sized_size(element_at(&list, index)) != list.stride) {
            return false;
        }
    }
    return true;
}

void weivel_element_read(const void *items, uint32_t count, uint32_t index, void *own, size_t own_size) {
    element_list list = list_of(items, count);
    weivel_sized_read(own, own_size, element_at(&list, index));
}

bool weivel_numbers_readable(const weivel_numbers *numbers) {
    if (!weivel_elements_readable(numbers->items, numbers->count, WEIVEL_NUMBER_FIRST_SIZE)) {
        return false;
    }
    element_list list = list_of(numbers->items, numbers->count);
    for (uint32_t index = 0; index < list.count; index++) {
        weivel_number number = number_at(&list, index);
        if (is_empty(number.name) || number_named_before(&list, index, number.name)) {
            return false;
        }
    }
    return true;
}

bool weivel_strings_copy(const weivel_strings *source, weivel_strings *copy, void **storage) {
    arena measuring = {NULL, 0};
    keep_strings(&measuring, source);
    arena filling = {calloc(1, measuring.used == 0 ? 1 : measuring.used), 0};
    *storage = filling.base;
    if (filling.base == NULL) {
        return false;
    }
    *copy = keep_strings(&filling, source);
    return true;
}
