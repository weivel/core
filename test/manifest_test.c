#include <stdlib.h>

#include "check.h"
#include "core_internal.h"

static weivel_core *create_core(void) {
    weivel_core *core = NULL;
    CHECK(weivel_core_create(NULL, &core, NULL) == WEIVEL_STATUS_OK);
    return core;
}

static const weivel_manifest *registered(weivel_core *core, weivel_handle handle) {
    void *record = NULL;
    CHECK(weivel_handles_resolve(core->handles, handle, WEIVEL_RECORD_PLUGIN, &record) == WEIVEL_STATUS_OK);
    return record == NULL ? NULL : &((weivel_plugin *)record)->manifest.value;
}

static weivel_number number_of(const char *name, double value) {
    weivel_number number = WEIVEL_INIT(weivel_number);
    number.name = name;
    number.value = value;
    return number;
}

/** Every buffer the caller's manifest points into, so the test can wreck and free each after registering. */
typedef struct callers_buffers {
    char *texts[16];
    uint32_t text_count;
    const char **lists[4];
    weivel_number numbers[2];
    weivel_service_offer offers[1];
    weivel_service_want wants[2];
} callers_buffers;

static const char *text(callers_buffers *buffers, const char *value) {
    size_t length = strlen(value) + 1;
    char *copy = malloc(length);
    memcpy(copy, value, length);
    buffers->texts[buffers->text_count++] = copy;
    return copy;
}

static weivel_strings list(callers_buffers *buffers, uint32_t slot, const char *first, const char *second) {
    const char **items = malloc(2 * sizeof *items);
    items[0] = text(buffers, first);
    items[1] = text(buffers, second);
    buffers->lists[slot] = items;
    weivel_strings strings = {items, 2};
    return strings;
}

static weivel_manifest full_manifest(callers_buffers *buffers) {
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = text(buffers, "media-thing");
    manifest.api = 3;
    manifest.entry = text(buffers, "dist/index");
    manifest.targets = list(buffers, 0, "studio", "canvas");
    manifest.capabilities.provides = list(buffers, 1, "media.decode", "media.encode");
    manifest.capabilities.consumes = list(buffers, 2, "storage.object", "storage.cache");
    manifest.permissions = list(buffers, 3, "network", "camera");

    buffers->numbers[0] = number_of(text(buffers, "maxReplicas"), 3);
    buffers->numbers[1] = number_of(text(buffers, "minReplicas"), 2);
    buffers->offers[0] = (weivel_service_offer)WEIVEL_INIT(weivel_service_offer);
    buffers->offers[0].id = text(buffers, "media:transcode");
    buffers->offers[0].attributes.items = &buffers->numbers[0];
    buffers->offers[0].attributes.count = 1;
    buffers->wants[0] = (weivel_service_want)WEIVEL_INIT(weivel_service_want);
    buffers->wants[0].id = text(buffers, "storage:object");
    buffers->wants[0].provider = text(buffers, "local:storage");
    buffers->wants[0].optional = WEIVEL_CHOICE_NO;
    buffers->wants[0].degradable = WEIVEL_CHOICE_YES;
    buffers->wants[0].constraints.items = &buffers->numbers[1];
    buffers->wants[0].constraints.count = 1;
    buffers->wants[1] = (weivel_service_want)WEIVEL_INIT(weivel_service_want);
    buffers->wants[1].id = text(buffers, "identity:accounts");
    manifest.services.provides.items = buffers->offers;
    manifest.services.provides.count = 1;
    manifest.services.consumes.items = buffers->wants;
    manifest.services.consumes.count = 2;
    return manifest;
}

static void wreck_and_free(callers_buffers *buffers, weivel_manifest *manifest) {
    for (uint32_t index = 0; index < buffers->text_count; index++) {
        memset(buffers->texts[index], 'x', strlen(buffers->texts[index]));
    }
    for (uint32_t index = 0; index < 4; index++) {
        buffers->lists[index][0] = "wrecked";
    }
    memset(buffers->numbers, 0xee, sizeof buffers->numbers);
    memset(buffers->offers, 0xee, sizeof buffers->offers);
    memset(buffers->wants, 0xee, sizeof buffers->wants);
    memset(manifest, 0xee, sizeof *manifest);
    for (uint32_t index = 0; index < buffers->text_count; index++) {
        free(buffers->texts[index]);
    }
    for (uint32_t index = 0; index < 4; index++) {
        free((void *)buffers->lists[index]);
    }
}

static void check_strings(const weivel_strings *strings, const char *first, const char *second, const char *label) {
    CHECK(strings->count == 2);
    if (strings->count == 2) {
        check_text(strings->items[0], first, label);
        check_text(strings->items[1], second, label);
    }
}

static void a_registered_manifest_is_a_copy_the_caller_may_free(void) {
    weivel_core *core = create_core();
    callers_buffers buffers;
    memset(&buffers, 0, sizeof buffers);
    weivel_manifest manifest = full_manifest(&buffers);
    weivel_handle handle = 0;
    CHECK(weivel_register_manifest(core, &manifest, &handle, NULL) == WEIVEL_STATUS_OK);
    CHECK(handle != 0);
    wreck_and_free(&buffers, &manifest);

    const weivel_manifest *kept = registered(core, handle);
    if (kept != NULL) {
        check_text(kept->import_name, "media-thing", "import_name");
        CHECK(kept->api == 3);
        check_text(kept->entry, "dist/index", "entry");
        check_strings(&kept->targets, "studio", "canvas", "targets");
        check_strings(&kept->capabilities.provides, "media.decode", "media.encode", "capabilities.provides");
        check_strings(&kept->capabilities.consumes, "storage.object", "storage.cache", "capabilities.consumes");
        check_strings(&kept->permissions, "network", "camera", "permissions");
        CHECK(kept->services.provides.count == 1);
        const weivel_service_offer *offer = &kept->services.provides.items[0];
        check_text(offer->id, "media:transcode", "offer id");
        CHECK(offer->attributes.count == 1);
        check_text(offer->attributes.items[0].name, "maxReplicas", "attribute name");
        CHECK(offer->attributes.items[0].value == 3);
        CHECK(kept->services.consumes.count == 2);
        const weivel_service_want *want = &kept->services.consumes.items[0];
        check_text(want->id, "storage:object", "want id");
        check_text(want->provider, "local:storage", "want provider");
        CHECK(want->optional == WEIVEL_CHOICE_NO);
        CHECK(want->degradable == WEIVEL_CHOICE_YES);
        CHECK(want->constraints.count == 1);
        check_text(want->constraints.items[0].name, "minReplicas", "constraint name");
        CHECK(want->constraints.items[0].value == 2);
    }
    weivel_core_destroy(core);
}

static void an_absent_choice_takes_the_models_default(void) {
    weivel_core *core = create_core();
    weivel_service_want wants[1] = {WEIVEL_INIT(weivel_service_want)};
    wants[0].id = "identity:accounts";
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "accounts-user";
    manifest.services.consumes.items = wants;
    manifest.services.consumes.count = 1;
    weivel_handle handle = 0;
    CHECK(weivel_register_manifest(core, &manifest, &handle, NULL) == WEIVEL_STATUS_OK);
    const weivel_manifest *kept = registered(core, handle);
    if (kept != NULL) {
        const weivel_service_want *want = &kept->services.consumes.items[0];
        CHECK(want->optional == WEIVEL_CHOICE_YES);
        CHECK(want->degradable == WEIVEL_CHOICE_NO);
        CHECK(want->provider == NULL);
        CHECK(want->constraints.count == 0);
        CHECK(kept->entry == NULL);
        CHECK(kept->targets.count == 0);
    }
    weivel_core_destroy(core);
}

typedef struct first_shape_want {
    uint32_t size;
    const char *id;
    const char *provider;
    uint32_t optional;
    uint32_t degradable;
} first_shape_want;

typedef struct first_shape_offer {
    uint32_t size;
    const char *id;
} first_shape_offer;

static void a_manifest_built_against_the_first_shape_still_loads(void) {
    weivel_core *core = create_core();
    first_shape_want wants[2] = {{sizeof(first_shape_want), "storage:object", NULL, WEIVEL_CHOICE_NO, 0},
                                 {sizeof(first_shape_want), "identity:accounts", NULL, 0, WEIVEL_CHOICE_YES}};
    first_shape_offer offers[2] = {{sizeof(first_shape_offer), "media:transcode"},
                                   {sizeof(first_shape_offer), "media:probe"}};
    weivel_manifest manifest;
    memset(&manifest, 0xee, sizeof manifest);
    manifest.size = (uint32_t)offsetof(weivel_manifest, permissions);
    manifest.import_name = "older-thing";
    manifest.api = 1;
    manifest.entry = NULL;
    manifest.targets.count = 0;
    manifest.capabilities.provides.count = 0;
    manifest.capabilities.consumes.count = 0;
    manifest.services.provides.items = (const weivel_service_offer *)(const void *)offers;
    manifest.services.provides.count = 2;
    manifest.services.consumes.items = (const weivel_service_want *)(const void *)wants;
    manifest.services.consumes.count = 2;

    weivel_handle handle = 0;
    CHECK(weivel_register_manifest(core, &manifest, &handle, NULL) == WEIVEL_STATUS_OK);
    const weivel_manifest *kept = registered(core, handle);
    if (kept != NULL) {
        CHECK(kept->permissions.count == 0);
        CHECK(kept->permissions.items == NULL);
        CHECK(kept->services.provides.count == 2);
        check_text(kept->services.provides.items[1].id, "media:probe", "second offer");
        CHECK(kept->services.provides.items[1].attributes.count == 0);
        CHECK(kept->services.consumes.count == 2);
        const weivel_service_want *second = &kept->services.consumes.items[1];
        check_text(second->id, "identity:accounts", "second want");
        CHECK(second->optional == WEIVEL_CHOICE_YES);
        CHECK(second->degradable == WEIVEL_CHOICE_YES);
        CHECK(second->constraints.count == 0);
        CHECK(kept->services.consumes.items[0].optional == WEIVEL_CHOICE_NO);
    }
    weivel_core_destroy(core);
}

typedef struct newer_want {
    weivel_service_want want;
    uint64_t appended_later;
} newer_want;

typedef struct newer_manifest {
    weivel_manifest manifest;
    uint64_t appended_later[2];
} newer_manifest;

static void a_manifest_from_a_newer_caller_is_read_up_to_what_the_core_knows(void) {
    weivel_core *core = create_core();
    newer_want wants[2];
    memset(wants, 0xee, sizeof wants);
    for (uint32_t index = 0; index < 2; index++) {
        wants[index].want = (weivel_service_want)WEIVEL_INIT(weivel_service_want);
        wants[index].want.size = (uint32_t)sizeof(newer_want);
    }
    wants[0].want.id = "storage:object";
    wants[1].want.id = "identity:accounts";
    wants[1].want.optional = WEIVEL_CHOICE_NO;
    newer_manifest newer;
    memset(&newer, 0xee, sizeof newer);
    newer.manifest = (weivel_manifest)WEIVEL_INIT(weivel_manifest);
    newer.manifest.size = (uint32_t)sizeof newer;
    newer.manifest.import_name = "newer-thing";
    newer.manifest.services.consumes.items = &wants[0].want;
    newer.manifest.services.consumes.count = 2;

    weivel_handle handle = 0;
    CHECK(weivel_register_manifest(core, &newer.manifest, &handle, NULL) == WEIVEL_STATUS_OK);
    const weivel_manifest *kept = registered(core, handle);
    if (kept != NULL) {
        CHECK(kept->size == sizeof(weivel_manifest));
        CHECK(kept->services.consumes.count == 2);
        check_text(kept->services.consumes.items[1].id, "identity:accounts", "second want");
        CHECK(kept->services.consumes.items[1].optional == WEIVEL_CHOICE_NO);
        CHECK(kept->services.consumes.items[1].size == sizeof(weivel_service_want));
    }
    weivel_core_destroy(core);
}

static void check_refusal_event(weivel_core *core, const char *detail, const char *fix) {
    weivel_event event = WEIVEL_INIT(weivel_event);
    CHECK(weivel_next_event(core, &event, NULL) == WEIVEL_STATUS_OK);
    CHECK(event.kind == WEIVEL_EVENT_REFUSAL);
    CHECK(event.data.refusal.code == WEIVEL_REFUSAL_MALFORMED_MANIFEST);
    check_text(event.data.refusal.detail, detail, "refusal detail");
    check_text(event.data.refusal.fix, fix, "refusal fix");
    weivel_event_release(&event);
}

static void check_shape_refused(const weivel_manifest *manifest, const char *detail, const char *fix) {
    weivel_core *core = create_core();
    weivel_handle handle = 77;
    uint32_t pending = 0;
    CHECK(weivel_register_manifest(core, manifest, &handle, &pending) == WEIVEL_STATUS_REFUSED);
    CHECK(handle == 0);
    CHECK(pending == 1);
    check_refusal_event(core, detail, fix);

    weivel_support answer = WEIVEL_INIT(weivel_support);
    CHECK(weivel_supports(core, manifest, &answer, &pending) == WEIVEL_STATUS_OK);
    CHECK(pending == 0);
    CHECK(answer.supported == 0);
    CHECK(answer.axis == WEIVEL_AXIS_NONE);
    CHECK(answer.refusal.code == WEIVEL_REFUSAL_MALFORMED_MANIFEST);
    check_text(answer.refusal.detail, detail, "answer detail");
    weivel_support_release(&answer);
    weivel_core_destroy(core);
}

static weivel_manifest named(const char *import_name) {
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = import_name;
    return manifest;
}

static void a_manifest_without_an_import_name_is_refused(void) {
    weivel_manifest manifest = named(NULL);
    check_shape_refused(&manifest, "the manifest has no import name", "set import_name to the plugin's id");
    manifest = named("");
    check_shape_refused(&manifest, "the manifest has no import name", "set import_name to the plugin's id");
}

static void a_null_in_a_list_is_refused(void) {
    const char *const holes[] = {"studio", NULL};
    weivel_manifest manifest = named("holey");
    manifest.targets.items = holes;
    manifest.targets.count = 2;
    check_shape_refused(&manifest, "the manifest of \"holey\" has no string at targets[1]",
                        "give targets[1] a string, or remove it");

    manifest = named("holey");
    manifest.permissions.count = 3;
    check_shape_refused(&manifest, "the manifest of \"holey\" counts 3 entries in permissions but points at none",
                        "point permissions's items at its entries, or set its count to 0");

    manifest = named("holey");
    manifest.services.consumes.count = 1;
    check_shape_refused(&manifest,
                        "the manifest of \"holey\" counts 1 entries in services.consumes but points at none",
                        "point services.consumes's items at its entries, or set its count to 0");
}

static void an_id_listed_twice_is_refused(void) {
    const char *const twice[] = {"media.decode", "media.encode", "media.decode"};
    weivel_manifest manifest = named("doubled");
    manifest.capabilities.provides.items = twice;
    manifest.capabilities.provides.count = 3;
    check_shape_refused(&manifest, "the manifest of \"doubled\" lists \"media.decode\" twice in capabilities.provides",
                        "list \"media.decode\" once in capabilities.provides");

    weivel_service_want wants[2] = {WEIVEL_INIT(weivel_service_want), WEIVEL_INIT(weivel_service_want)};
    wants[0].id = "storage:object";
    wants[1].id = "storage:object";
    manifest = named("doubled");
    manifest.services.consumes.items = wants;
    manifest.services.consumes.count = 2;
    check_shape_refused(&manifest, "the manifest of \"doubled\" lists \"storage:object\" twice in services.consumes",
                        "list \"storage:object\" once in services.consumes");

    weivel_number numbers[2] = {number_of("minReplicas", 1), number_of("minReplicas", 2)};
    weivel_service_offer offers[1] = {WEIVEL_INIT(weivel_service_offer)};
    offers[0].id = "media:transcode";
    offers[0].attributes.items = numbers;
    offers[0].attributes.count = 2;
    manifest = named("doubled");
    manifest.services.provides.items = offers;
    manifest.services.provides.count = 1;
    check_shape_refused(&manifest,
                        "the manifest of \"doubled\" lists \"minReplicas\" twice in services.provides[0].attributes",
                        "list \"minReplicas\" once in services.provides[0].attributes");
}

static void a_service_without_an_id_is_refused(void) {
    weivel_service_offer offers[2] = {WEIVEL_INIT(weivel_service_offer), WEIVEL_INIT(weivel_service_offer)};
    offers[0].id = "media:transcode";
    weivel_manifest manifest = named("nameless");
    manifest.services.provides.items = offers;
    manifest.services.provides.count = 2;
    check_shape_refused(&manifest, "the manifest of \"nameless\" has no id at services.provides[1]",
                        "give services.provides[1] an id, or remove it");
}

static void a_choice_out_of_range_is_refused(void) {
    weivel_service_want wants[1] = {WEIVEL_INIT(weivel_service_want)};
    wants[0].id = "storage:object";
    wants[0].degradable = 7;
    weivel_manifest manifest = named("choosy");
    manifest.services.consumes.items = wants;
    manifest.services.consumes.count = 1;
    check_shape_refused(&manifest,
                        "the manifest of \"choosy\" sets services.consumes[0].degradable to 7, which is no "
                        "weivel_choice",
                        "set it to WEIVEL_CHOICE_DEFAULT, WEIVEL_CHOICE_NO or WEIVEL_CHOICE_YES");
}

static void elements_of_a_wrong_size_are_refused(void) {
    weivel_service_want wants[2] = {WEIVEL_INIT(weivel_service_want), WEIVEL_INIT(weivel_service_want)};
    wants[0].id = "storage:object";
    wants[1].id = "identity:accounts";
    weivel_manifest manifest = named("sized");
    manifest.services.consumes.items = wants;
    manifest.services.consumes.count = 2;

    wants[0].size = 8;
    wants[1].size = 8;
    char detail[256];
    snprintf(detail, sizeof detail,
             "the manifest of \"sized\" gives services.consumes elements of size 8, smaller than their first shape "
             "of %u",
             (unsigned)WEIVEL_SERVICE_WANT_FIRST_SIZE);
    check_shape_refused(&manifest, detail, "initialise each element with WEIVEL_INIT");

    wants[0].size = (uint32_t)sizeof(weivel_service_want);
    wants[1].size = (uint32_t)WEIVEL_SERVICE_WANT_FIRST_SIZE;
    snprintf(detail, sizeof detail,
             "the manifest of \"sized\" gives services.consumes[1] size %u where services.consumes[0] gives %u",
             (unsigned)WEIVEL_SERVICE_WANT_FIRST_SIZE, (unsigned)sizeof(weivel_service_want));
    check_shape_refused(&manifest, detail, "build each list as one array of one element type");
}

static void malformed_register_calls_are_answered_not_crashed(void) {
    weivel_core *core = create_core();
    weivel_manifest manifest = named("fine");
    weivel_handle handle = 77;
    uint32_t pending = 0;
    CHECK(weivel_register_manifest(NULL, &manifest, &handle, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(handle == 77);
    CHECK(weivel_register_manifest(core, NULL, &handle, &pending) == WEIVEL_STATUS_MALFORMED);
    CHECK(handle == 0);
    CHECK(pending == 1);
    CHECK(weivel_register_manifest(core, &manifest, NULL, &pending) == WEIVEL_STATUS_MALFORMED);
    CHECK(pending == 2);
    manifest.size = (uint32_t)offsetof(weivel_manifest, services);
    CHECK(weivel_register_manifest(core, &manifest, &handle, &pending) == WEIVEL_STATUS_MALFORMED);
    CHECK(pending == 3);
    weivel_core_destroy(core);
}

static void each_registration_answers_its_own_handle(void) {
    weivel_core *core = create_core();
    weivel_manifest first = named("first");
    weivel_manifest second = named("second");
    weivel_handle first_handle = 0;
    weivel_handle second_handle = 0;
    for (uint32_t round = 0; round < 40; round++) {
        CHECK(weivel_register_manifest(core, &first, &first_handle, NULL) == WEIVEL_STATUS_OK);
    }
    CHECK(weivel_register_manifest(core, &second, &second_handle, NULL) == WEIVEL_STATUS_OK);
    CHECK(first_handle != second_handle);
    const weivel_manifest *kept = registered(core, second_handle);
    if (kept != NULL) {
        check_text(kept->import_name, "second", "second import name");
    }
    weivel_core_destroy(core);
}

int main(void) {
    a_registered_manifest_is_a_copy_the_caller_may_free();
    an_absent_choice_takes_the_models_default();
    a_manifest_built_against_the_first_shape_still_loads();
    a_manifest_from_a_newer_caller_is_read_up_to_what_the_core_knows();
    a_manifest_without_an_import_name_is_refused();
    a_null_in_a_list_is_refused();
    an_id_listed_twice_is_refused();
    a_service_without_an_id_is_refused();
    a_choice_out_of_range_is_refused();
    elements_of_a_wrong_size_are_refused();
    malformed_register_calls_are_answered_not_crashed();
    each_registration_answers_its_own_handle();
    return check_summary("manifest_test");
}
