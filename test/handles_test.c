#include "check.h"
#include "handles.h"

#define RECORD_KIND 1u
#define OTHER_KIND 2u

static void a_released_handle_is_refused_after_its_slot_is_reused(void) {
    weivel_handles *handles = weivel_handles_create();
    int first_record = 1;
    int second_record = 2;
    weivel_handle old_handle = 0;
    weivel_handle new_handle = 0;
    void *record = NULL;

    CHECK(weivel_handles_allocate(handles, RECORD_KIND, &first_record, &old_handle) == WEIVEL_STATUS_OK);
    CHECK(weivel_handles_release(handles, old_handle, RECORD_KIND) == WEIVEL_STATUS_OK);
    CHECK(weivel_handles_allocate(handles, RECORD_KIND, &second_record, &new_handle) == WEIVEL_STATUS_OK);

    CHECK((old_handle & 0xffffffffu) == (new_handle & 0xffffffffu));
    CHECK(old_handle != new_handle);
    CHECK(weivel_handles_resolve(handles, old_handle, RECORD_KIND, &record) == WEIVEL_STATUS_REFUSED);
    CHECK(record == NULL);
    CHECK(weivel_handles_release(handles, old_handle, RECORD_KIND) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_handles_resolve(handles, new_handle, RECORD_KIND, &record) == WEIVEL_STATUS_OK);
    CHECK(record == &second_record);
    weivel_handles_destroy(handles);
}

static void handle_zero_is_never_issued_and_always_refused(void) {
    weivel_handles *handles = weivel_handles_create();
    int value = 0;
    void *record = &value;
    weivel_handle handle = 0;

    CHECK(weivel_handles_resolve(handles, 0, RECORD_KIND, &record) == WEIVEL_STATUS_REFUSED);
    CHECK(record == NULL);
    for (int index = 0; index < 100; index++) {
        CHECK(weivel_handles_allocate(handles, RECORD_KIND, &value, &handle) == WEIVEL_STATUS_OK);
        CHECK(handle != 0);
    }
    CHECK(weivel_handles_resolve(handles, 0, RECORD_KIND, &record) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_handles_release(handles, 0, RECORD_KIND) == WEIVEL_STATUS_REFUSED);
    weivel_handles_destroy(handles);
}

static void a_handle_never_issued_or_of_another_kind_is_refused(void) {
    weivel_handles *handles = weivel_handles_create();
    int value = 0;
    void *record = NULL;
    weivel_handle handle = 0;

    CHECK(weivel_handles_allocate(handles, RECORD_KIND, &value, &handle) == WEIVEL_STATUS_OK);
    CHECK(weivel_handles_resolve(handles, handle, OTHER_KIND, &record) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_handles_resolve(handles, handle + 1, RECORD_KIND, &record) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_handles_resolve(handles, handle + ((weivel_handle)1 << 32), RECORD_KIND, &record) ==
          WEIVEL_STATUS_REFUSED);
    CHECK(weivel_handles_release(handles, handle, OTHER_KIND) == WEIVEL_STATUS_REFUSED);
    CHECK(weivel_handles_resolve(handles, handle, RECORD_KIND, &record) == WEIVEL_STATUS_OK);
    weivel_handles_destroy(handles);
}

static void many_handles_stay_distinct_across_growth(void) {
    weivel_handles *handles = weivel_handles_create();
    static int records[1000];
    static weivel_handle issued[1000];
    void *record = NULL;

    for (int index = 0; index < 1000; index++) {
        CHECK(weivel_handles_allocate(handles, RECORD_KIND, &records[index], &issued[index]) == WEIVEL_STATUS_OK);
    }
    for (int index = 0; index < 1000; index++) {
        CHECK(weivel_handles_resolve(handles, issued[index], RECORD_KIND, &record) == WEIVEL_STATUS_OK);
        CHECK(record == &records[index]);
    }
    weivel_handles_destroy(handles);
}

static void a_slot_at_its_last_generation_is_retired(void) {
    CHECK(weivel_handles_next_generation(1) == 2);
    CHECK(weivel_handles_next_generation(UINT32_MAX - 1) == UINT32_MAX);
    CHECK(weivel_handles_next_generation(UINT32_MAX) == 0);
}

int main(void) {
    a_released_handle_is_refused_after_its_slot_is_reused();
    handle_zero_is_never_issued_and_always_refused();
    a_handle_never_issued_or_of_another_kind_is_refused();
    many_handles_stay_distinct_across_growth();
    a_slot_at_its_last_generation_is_retired();
    return check_summary("handles_test");
}
