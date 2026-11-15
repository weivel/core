#include <string.h>

#include "check.h"
#include "echo/echo.h"

typedef struct body {
    int32_t status;
    uint8_t *bytes;
    size_t length;
} body;

typedef int32_t (*echo_call)(const uint8_t *, size_t, uint8_t **, size_t *);

static body call(echo_call function, const char *input) {
    body answered = {99, NULL, 0};
    answered.status = function((const uint8_t *)input, strlen(input), &answered.bytes, &answered.length);
    return answered;
}

static void echo_answers_its_input(void) {
    body answered = call(echo_echo, "hello");
    CHECK(answered.status == 0);
    CHECK(answered.length == 5 && memcmp(answered.bytes, "hello", 5) == 0);
    echo_release(answered.bytes);

    body empty = call(echo_echo, "");
    CHECK(empty.status == 0);
    CHECK(empty.length == 0 && empty.bytes != NULL);
    echo_release(empty.bytes);
}

static void fail_refuses_with_a_text(void) {
    body answered = call(echo_fail, "anything");
    CHECK(answered.status == 1);
    CHECK(answered.length == strlen("echo refused on purpose"));
    CHECK(answered.bytes != NULL && memcmp(answered.bytes, "echo refused on purpose", answered.length) == 0);
    echo_release(answered.bytes);
}

static void garbage_answers_bytes_that_are_not_text(void) {
    body answered = call(echo_garbage, "");
    CHECK(answered.status == 0);
    CHECK(answered.length == 2 && answered.bytes[0] == 0xFF && answered.bytes[1] == 0xFE);
    echo_release(answered.bytes);
    echo_release(NULL);
}

static uint64_t answered_number(echo_call function, const uint8_t *input, size_t input_length) {
    body answered = {99, NULL, 0};
    uint64_t number = 0;
    answered.status = function(input, input_length, &answered.bytes, &answered.length);
    CHECK(answered.status == 0);
    CHECK(answered.length == sizeof number && answered.bytes != NULL);
    if (answered.length == sizeof number && answered.bytes != NULL) {
        memcpy(&number, answered.bytes, sizeof number);
    }
    echo_release(answered.bytes);
    return number;
}

static void where_answers_the_address_it_was_handed(void) {
    static const uint8_t first[] = {1, 2, 3};
    static const uint8_t second[] = {4, 5, 6, 7};
    CHECK(answered_number(echo_where, first, sizeof first) == (uint64_t)(uintptr_t)first);
    CHECK(answered_number(echo_where, second, sizeof second) == (uint64_t)(uintptr_t)second);
    CHECK(answered_number(echo_where, second + 1, 0) == (uint64_t)(uintptr_t)(second + 1));
}

static void released_counts_every_body_released(void) {
    uint64_t before = answered_number(echo_released, NULL, 0);
    body echoed = call(echo_echo, "counted");
    echo_release(echoed.bytes);
    CHECK(answered_number(echo_released, NULL, 0) == before + 2);
}

static void released_does_not_count_a_null_release(void) {
    uint64_t before = answered_number(echo_released, NULL, 0);
    echo_release(NULL);
    CHECK(answered_number(echo_released, NULL, 0) == before + 1);
}

static const echo_definition *definition(void) {
    return (const echo_definition *)echo_service_definition();
}

static void a_definition_is_one_static_struct_of_its_own_size(void) {
    const echo_definition *answered = definition();
    CHECK(answered != NULL);
    CHECK(answered == definition());
    CHECK(answered->size == sizeof(echo_definition));
    CHECK(answered->echo != NULL && answered->release != NULL && answered->count_unloads != NULL);
}

static void a_definition_echoes_through_its_pointers(void) {
    const echo_definition *answered = definition();
    body echoed = {99, NULL, 0};
    echoed.status = answered->echo(answered->context, (const uint8_t *)"through", 7, &echoed.bytes, &echoed.length);
    CHECK(echoed.status == 0);
    CHECK(echoed.length == 7 && echoed.bytes != NULL && memcmp(echoed.bytes, "through", 7) == 0);
    answered->release(answered->context, echoed.bytes);
}

static void a_definition_refuses_a_context_not_its_own(void) {
    const echo_definition *answered = definition();
    int foreign = 0;
    body refused = {99, NULL, 0};
    refused.status = answered->echo(&foreign, (const uint8_t *)"x", 1, &refused.bytes, &refused.length);
    CHECK(refused.status == 1);
    CHECK(refused.length == strlen("echo refused on purpose"));
    answered->release(answered->context, refused.bytes);
}

static void a_definition_release_is_counted(void) {
    const echo_definition *answered = definition();
    uint64_t before = answered_number(echo_released, NULL, 0);
    body echoed = {99, NULL, 0};
    echoed.status = answered->echo(answered->context, (const uint8_t *)"c", 1, &echoed.bytes, &echoed.length);
    answered->release(answered->context, echoed.bytes);
    answered->release(answered->context, NULL);
    CHECK(answered_number(echo_released, NULL, 0) == before + 2);
}

static void a_null_definition_answers_null(void) {
    CHECK(echo_null_definition() == NULL);
}

static void a_short_definition_answers_a_size_below_a_size_and_one_pointer(void) {
    const uint32_t *size = (const uint32_t *)echo_short_definition();
    CHECK(size != NULL && *size == 4 && *size < 2 * sizeof(void *));
}

int main(void) {
    a_definition_is_one_static_struct_of_its_own_size();
    a_definition_echoes_through_its_pointers();
    a_definition_refuses_a_context_not_its_own();
    a_definition_release_is_counted();
    a_null_definition_answers_null();
    a_short_definition_answers_a_size_below_a_size_and_one_pointer();
    echo_answers_its_input();
    fail_refuses_with_a_text();
    garbage_answers_bytes_that_are_not_text();
    where_answers_the_address_it_was_handed();
    released_counts_every_body_released();
    released_does_not_count_a_null_release();
    return check_summary("echo_test");
}
