#include "check.h"
#include "sized.h"

typedef struct full_shape {
    uint32_t size;
    uint32_t first;
    uint32_t appended;
    uint32_t appended_again;
} full_shape;

static void a_read_copies_only_the_covered_fields_and_zeroes_the_rest(void) {
    full_shape callers = {.size = 8, .first = 7, .appended = 9, .appended_again = 9};
    full_shape own;
    memset(&own, 0xee, sizeof own);
    weivel_sized_read(&own, sizeof own, &callers);
    CHECK(own.first == 7);
    CHECK(own.appended == 0);
    CHECK(own.appended_again == 0);
}

static void a_write_stops_at_the_callers_size(void) {
    full_shape own = {.size = sizeof(full_shape), .first = 1, .appended = 2, .appended_again = 3};
    full_shape callers;
    memset(&callers, 0xee, sizeof callers);
    callers.size = 12;
    weivel_sized_write(&callers, &own, sizeof own);
    CHECK(callers.size == 12);
    CHECK(callers.first == 1);
    CHECK(callers.appended == 2);
    CHECK(callers.appended_again == 0xeeeeeeeeu);
}

int main(void) {
    a_read_copies_only_the_covered_fields_and_zeroes_the_rest();
    a_write_stops_at_the_callers_size();
    return check_summary("sized_test");
}
