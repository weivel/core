#include <weivel/core.h>

#include "registry_support.h"

static const char *const well_known[] = {"shared"};

typedef struct answer {
    weivel_status status;
    uint32_t condition;
    uint32_t pending_before;
    uint32_t pending_after;
} answer;

static uint32_t pending_of(weivel_core *core) {
    int64_t deadline = 0;
    uint32_t pending = 0;
    weivel_next_deadline(core, &deadline, &pending);
    return pending;
}

static answer state_of(weivel_core *core, weivel_handle handle) {
    answer read;
    read.pending_before = pending_of(core);
    read.condition = 99;
    read.status = weivel_handle_state(core, handle, &read.condition, &read.pending_after);
    return read;
}

static void check_answer(weivel_core *core, weivel_handle handle, weivel_status status, uint32_t condition) {
    answer read = state_of(core, handle);
    CHECK(read.status == status);
    CHECK(read.condition == condition);
    CHECK(read.pending_after == read.pending_before);
}

static weivel_core *fresh_core(void) {
    return core_knowing(well_known, 1);
}

static void a_want_waits_settles_or_is_withdrawn(void) {
    weivel_core *core = fresh_core();
    plugin_spec spec = {.identity = "asker", .consumes = {"shared", "other:thing", "third:thing"}};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_handle waiting = want_untimed(core, plugin, "shared");
    check_answer(core, waiting, WEIVEL_STATUS_OK, WEIVEL_HANDLE_WAITING);
    register_service(core, WEIVEL_HOST, "shared");
    check_answer(core, waiting, WEIVEL_STATUS_OK, WEIVEL_HANDLE_SETTLED);
    drain(core);
    check_answer(core, waiting, WEIVEL_STATUS_OK, WEIVEL_HANDLE_SETTLED);

    weivel_handle withdrawn = want_untimed(core, plugin, "other:thing");
    CHECK(weivel_unregister(core, withdrawn, NULL) == WEIVEL_STATUS_OK);
    check_answer(core, withdrawn, WEIVEL_STATUS_OK, WEIVEL_HANDLE_WITHDRAWN);

    weivel_handle stopped = want_untimed(core, plugin, "third:thing");
    check_answer(core, stopped, WEIVEL_STATUS_OK, WEIVEL_HANDLE_WAITING);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    check_answer(core, stopped, WEIVEL_STATUS_OK, WEIVEL_HANDLE_SETTLED);
    weivel_core_destroy(core);
}

static void a_want_rejected_at_once_has_settled(void) {
    weivel_core *core = fresh_core();
    plugin_spec spec = {.identity = "asker"};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_handle rejected = want_untimed(core, plugin, "elsewhere:thing");
    check_answer(core, rejected, WEIVEL_STATUS_OK, WEIVEL_HANDLE_SETTLED);
    weivel_core_destroy(core);
}

static weivel_handle watch_of(weivel_core *core, weivel_handle plugin, const char *id) {
    weivel_handle watch = 0;
    CHECK(weivel_watch_service(core, plugin, id, &watch, NULL) == WEIVEL_STATUS_OK);
    return watch;
}

static void a_watch_is_live_until_it_stops(void) {
    weivel_core *core = fresh_core();
    plugin_spec spec = {.identity = "watcher", .consumes = {"shared", "other:thing"}};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_handle ended = watch_of(core, plugin, "shared");
    weivel_handle released = watch_of(core, plugin, "other:thing");
    check_answer(core, ended, WEIVEL_STATUS_OK, WEIVEL_HANDLE_WATCHING);
    CHECK(weivel_unregister(core, ended, NULL) == WEIVEL_STATUS_OK);
    check_answer(core, ended, WEIVEL_STATUS_OK, WEIVEL_HANDLE_STOPPED);
    check_answer(core, released, WEIVEL_STATUS_OK, WEIVEL_HANDLE_WATCHING);
    CHECK(weivel_release_plugin(core, plugin, NULL) == WEIVEL_STATUS_OK);
    check_answer(core, released, WEIVEL_STATUS_OK, WEIVEL_HANDLE_STOPPED);
    weivel_core_destroy(core);
}

static void a_capability_watch_is_live_until_its_plugin_is_quarantined(void) {
    weivel_core *core = fresh_core();
    const char *const consumes[] = {"screens"};
    weivel_manifest manifest = WEIVEL_INIT(weivel_manifest);
    manifest.import_name = "viewer";
    manifest.api = 1;
    manifest.capabilities.consumes.items = consumes;
    manifest.capabilities.consumes.count = 1;
    weivel_handle plugin = 0;
    CHECK(weivel_register_manifest(core, &manifest, &plugin, NULL) == WEIVEL_STATUS_OK);
    CHECK(weivel_open_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    weivel_handle watch = 0;
    CHECK(weivel_watch_capability(core, plugin, "screens", &watch, NULL) == WEIVEL_STATUS_OK);
    check_answer(core, watch, WEIVEL_STATUS_OK, WEIVEL_HANDLE_WATCHING);
    CHECK(weivel_quarantine_plugin(core, plugin, NULL, NULL) == WEIVEL_STATUS_OK);
    check_answer(core, watch, WEIVEL_STATUS_OK, WEIVEL_HANDLE_STOPPED);
    weivel_core_destroy(core);
}

static void a_handle_that_names_no_want_or_watch_is_refused_without_an_event(void) {
    weivel_core *core = fresh_core();
    plugin_spec spec = {.identity = "owner"};
    weivel_handle plugin = open_spec(core, &spec);
    weivel_handle registration = register_service(core, plugin, "owner:thing");
    forget(core);
    const weivel_handle refused[] = {0, (weivel_handle)12345, plugin, registration, WEIVEL_HOST};
    for (uint32_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        check_answer(core, refused[index], WEIVEL_STATUS_REFUSED, 0);
    }
    drain(core);
    CHECK(the_journal.count == 0);
    weivel_core_destroy(core);
}

static void an_ended_want_is_forgotten_once_its_place_ends_again(void) {
    weivel_core *core = fresh_core();
    plugin_spec spec = {.identity = "asker", .consumes = {"shared"}};
    weivel_handle plugin = open_spec(core, &spec);
    register_service(core, WEIVEL_HOST, "shared");
    weivel_handle first = want_untimed(core, plugin, "shared");
    weivel_handle second = want_untimed(core, plugin, "shared");
    CHECK((first & 0xffffffffu) == (second & 0xffffffffu));
    check_answer(core, second, WEIVEL_STATUS_OK, WEIVEL_HANDLE_SETTLED);
    check_answer(core, first, WEIVEL_STATUS_REFUSED, 0);
    weivel_core_destroy(core);
}

static void a_query_without_a_place_to_answer_is_malformed(void) {
    weivel_core *core = fresh_core();
    CHECK(weivel_handle_state(NULL, 1, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    forget(core);
    CHECK(weivel_handle_state(core, 1, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    drain(core);
    CHECK(count_kind(WEIVEL_EVENT_REFUSAL) == 1);
    CHECK(nth_of(WEIVEL_EVENT_REFUSAL, 0)->code == WEIVEL_REFUSAL_MALFORMED_CALL);
    weivel_core_destroy(core);
}

int main(void) {
    a_want_waits_settles_or_is_withdrawn();
    a_want_rejected_at_once_has_settled();
    a_watch_is_live_until_it_stops();
    a_capability_watch_is_live_until_its_plugin_is_quarantined();
    a_handle_that_names_no_want_or_watch_is_refused_without_an_event();
    an_ended_want_is_forgotten_once_its_place_ends_again();
    a_query_without_a_place_to_answer_is_malformed();
    return check_summary("handle_state_test");
}
