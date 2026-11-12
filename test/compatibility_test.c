#include <stddef.h>
#include <stdlib.h>
#include <weivel/core.h>

#include "check.h"

static weivel_strings strings_of(const char *const *items, uint32_t count) {
    weivel_strings strings = {items, count};
    return strings;
}

static weivel_core *host_of(const char *const *applications, uint32_t count, uint32_t api) {
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.applications = strings_of(applications, count);
    options.api = api;
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);
    return core;
}

static weivel_manifest manifest_of(const char *import_name, uint32_t api, const char *const *targets,
                                   uint32_t count) {
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = import_name;
    manifest.api = api;
    manifest.targets = strings_of(targets, count);
    return manifest;
}

static weivel_support supports(weivel_core *core, const weivel_manifest *manifest) {
    weivel_support answer = WEIVEL_INIT(weivel_support);
    CHECK(weivel_supports(core, manifest, &answer, NULL) == WEIVEL_STATUS_OK);
    return answer;
}

static void check_supported(weivel_core *core, const weivel_manifest *manifest) {
    weivel_support answer = supports(core, manifest);
    CHECK(answer.supported == 1);
    CHECK(answer.axis == WEIVEL_AXIS_NONE);
    CHECK(answer.refusal.code == WEIVEL_REFUSAL_NONE);
    CHECK(answer.refusal.detail == NULL);
    CHECK(answer.refusal.fix == NULL);
    CHECK(weivel_support_release(&answer) == WEIVEL_STATUS_OK);
}

static void check_refused(weivel_core *core, const weivel_manifest *manifest, uint32_t axis, const char *detail,
                          const char *fix) {
    weivel_support answer = supports(core, manifest);
    CHECK(answer.supported == 0);
    CHECK(answer.axis == axis);
    CHECK(answer.refusal.code == WEIVEL_REFUSAL_INCOMPATIBLE);
    check_text(answer.refusal.detail, detail, "detail");
    check_text(answer.refusal.fix, fix, "fix");
    CHECK(weivel_support_release(&answer) == WEIVEL_STATUS_OK);
    CHECK(answer.storage == NULL);
    CHECK(answer.refusal.detail == NULL);
}

static const char *const notes[] = {"notes"};
static const char *const studio[] = {"studio"};

static void another_application_is_refused_on_the_application_axis(void) {
    weivel_core *core = host_of(notes, 1, 2);
    weivel_manifest manifest = manifest_of("studio-thing", 1, studio, 1);
    check_refused(core, &manifest, WEIVEL_AXIS_APPLICATION, "is built for studio, and this host hosts notes",
                  "install it in studio, or add \"notes\" to \"targets\" in its manifest");
    weivel_core_destroy(core);
}

static void the_application_is_refused_before_the_api_floor(void) {
    weivel_core *core = host_of(notes, 1, 1);
    weivel_manifest manifest = manifest_of("studio-thing", 2, studio, 1);
    check_refused(core, &manifest, WEIVEL_AXIS_APPLICATION, "is built for studio, and this host hosts notes",
                  "install it in studio, or add \"notes\" to \"targets\" in its manifest");
    weivel_core_destroy(core);
}

static void an_api_above_the_hosts_is_refused_on_the_api_axis(void) {
    weivel_core *core = host_of(notes, 1, 1);
    weivel_manifest manifest = manifest_of("notes-thing", 2, notes, 1);
    check_refused(core, &manifest, WEIVEL_AXIS_API, "needs api 2, this host has api 1",
                  "update the app to a version that implements api 2 or later");
    manifest.api = 1;
    check_supported(core, &manifest);
    weivel_core_destroy(core);
}

static void either_side_naming_no_application_means_any(void) {
    weivel_core *anywhere = host_of(NULL, 0, 2);
    weivel_manifest targeted = manifest_of("studio-thing", 1, studio, 1);
    check_supported(anywhere, &targeted);
    weivel_core_destroy(anywhere);

    weivel_core *core = host_of(notes, 1, 2);
    weivel_manifest untargeted = manifest_of("anywhere-thing", 1, NULL, 0);
    check_supported(core, &untargeted);
    weivel_core_destroy(core);
}

static void overlapping_application_sets_are_supported(void) {
    const char *const both[] = {"notes", "studio"};
    weivel_core *core = host_of(both, 2, 2);
    weivel_manifest manifest = manifest_of("studio-thing", 1, studio, 1);
    check_supported(core, &manifest);
    weivel_core_destroy(core);
}

static void every_target_and_application_is_named(void) {
    const char *const hosted[] = {"notes", "editor"};
    const char *const targets[] = {"studio", "canvas"};
    weivel_core *core = host_of(hosted, 2, 2);
    weivel_manifest manifest = manifest_of("studio-thing", 1, targets, 2);
    check_refused(core, &manifest, WEIVEL_AXIS_APPLICATION,
                  "is built for studio, canvas, and this host hosts notes, editor",
                  "install it in studio, or add \"notes\" to \"targets\" in its manifest");
    weivel_core_destroy(core);
}

static char *copied(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    memcpy(copy, text, length);
    return copy;
}

static void the_hosts_applications_are_copied_at_create(void) {
    char *application = copied("notes");
    const char **applications = malloc(sizeof *applications);
    applications[0] = application;
    weivel_core *core = host_of(applications, 1, 2);
    memset(application, 'x', strlen(application));
    applications[0] = "studio";
    free(application);
    free((void *)applications);

    weivel_manifest manifest = manifest_of("studio-thing", 1, studio, 1);
    check_refused(core, &manifest, WEIVEL_AXIS_APPLICATION, "is built for studio, and this host hosts notes",
                  "install it in studio, or add \"notes\" to \"targets\" in its manifest");
    weivel_core_destroy(core);
}

static void options_of_the_previous_size_describe_a_host_of_everything_at_the_engines_api(void) {
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.applications = strings_of(notes, 1);
    options.api = 9;
    options.size = (uint32_t)offsetof(weivel_core_options, api);
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_OK);

    weivel_manifest manifest = manifest_of("studio-thing", WEIVEL_API_VERSION, studio, 1);
    check_supported(core, &manifest);
    manifest.api = 3;
    check_refused(core, &manifest, WEIVEL_AXIS_API, "needs api 3, this host has api 2",
                  "update the app to a version that implements api 3 or later");
    weivel_core_destroy(core);
}

static void a_host_that_names_no_api_implements_the_engines(void) {
    weivel_core *core = host_of(NULL, 0, 0);
    weivel_manifest manifest = manifest_of("current-thing", 2, NULL, 0);
    check_supported(core, &manifest);
    weivel_core_destroy(core);
}

static void a_host_with_a_null_application_is_malformed(void) {
    const char *const holes[] = {"notes", NULL};
    weivel_core_options options = WEIVEL_INIT(weivel_core_options);
    options.applications = strings_of(holes, 2);
    weivel_core *core = NULL;
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(core == NULL);
    options.applications = strings_of(NULL, 1);
    CHECK(weivel_core_create(&options, &core, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(core == NULL);
}

static void a_malformed_manifest_is_answered_as_data(void) {
    weivel_core *core = host_of(notes, 1, 2);
    weivel_manifest manifest = manifest_of("", 1, notes, 1);
    weivel_support answer = supports(core, &manifest);
    CHECK(answer.supported == 0);
    CHECK(answer.axis == WEIVEL_AXIS_NONE);
    CHECK(answer.refusal.code == WEIVEL_REFUSAL_MALFORMED_MANIFEST);
    check_text(answer.refusal.detail, "the manifest has no import name", "detail");
    check_text(answer.refusal.fix, "give the manifest an import name", "fix");
    weivel_support_release(&answer);
    weivel_core_destroy(core);
}

static void malformed_supports_calls_are_answered_not_crashed(void) {
    weivel_core *core = host_of(notes, 1, 2);
    weivel_manifest manifest = manifest_of("notes-thing", 1, notes, 1);
    weivel_support answer = WEIVEL_INIT(weivel_support);
    uint32_t pending = 0;

    CHECK(weivel_supports(NULL, &manifest, &answer, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_supports(core, NULL, &answer, &pending) == WEIVEL_STATUS_MALFORMED);
    CHECK(pending == 1);
    CHECK(weivel_supports(core, &manifest, NULL, &pending) == WEIVEL_STATUS_MALFORMED);
    CHECK(pending == 2);
    weivel_manifest small = manifest;
    small.size = (uint32_t)offsetof(weivel_manifest, services);
    CHECK(weivel_supports(core, &small, &answer, &pending) == WEIVEL_STATUS_MALFORMED);
    CHECK(pending == 3);
    weivel_support small_answer = WEIVEL_INIT(weivel_support);
    small_answer.size = (uint32_t)offsetof(weivel_support, storage);
    CHECK(weivel_supports(core, &manifest, &small_answer, &pending) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_support_release(&small_answer) == WEIVEL_STATUS_MALFORMED);
    CHECK(weivel_support_release(NULL) == WEIVEL_STATUS_MALFORMED);

    weivel_manifest elsewhere = manifest_of("studio-thing", 1, studio, 1);
    CHECK(weivel_supports(core, &elsewhere, &answer, NULL) == WEIVEL_STATUS_OK);
    const char *held = answer.refusal.detail;
    CHECK(weivel_supports(core, &manifest, &answer, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(answer.refusal.detail == held);
    CHECK(weivel_support_release(&answer) == WEIVEL_STATUS_OK);
    weivel_support zeroed;
    memset(&zeroed, 0, sizeof zeroed);
    CHECK(weivel_support_release(&zeroed) == WEIVEL_STATUS_OK);
    weivel_core_destroy(core);
}

int main(void) {
    another_application_is_refused_on_the_application_axis();
    the_application_is_refused_before_the_api_floor();
    an_api_above_the_hosts_is_refused_on_the_api_axis();
    either_side_naming_no_application_means_any();
    overlapping_application_sets_are_supported();
    every_target_and_application_is_named();
    the_hosts_applications_are_copied_at_create();
    options_of_the_previous_size_describe_a_host_of_everything_at_the_engines_api();
    a_host_that_names_no_api_implements_the_engines();
    a_host_with_a_null_application_is_malformed();
    a_malformed_manifest_is_answered_as_data();
    malformed_supports_calls_are_answered_not_crashed();
    return check_summary("compatibility_test");
}
