#include "echo.h"

#include <stdlib.h>
#include <string.h>

static const char refusal[] = "echo refused on purpose";

static uint64_t released_count = 0;

static int32_t emit(const uint8_t *bytes, size_t length, int32_t status, uint8_t **output, size_t *output_length) {
    uint8_t *copy = malloc(length == 0 ? 1 : length);
    if (copy == NULL) {
        *output = NULL;
        *output_length = 0;
        return 2;
    }
    if (length > 0) {
        memcpy(copy, bytes, length);
    }
    *output = copy;
    *output_length = length;
    return status;
}

int32_t echo_echo(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length) {
    return emit(input, input_length, 0, output, output_length);
}

int32_t echo_fail(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length) {
    (void)input;
    (void)input_length;
    return emit((const uint8_t *)refusal, sizeof refusal - 1, 1, output, output_length);
}

int32_t echo_garbage(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length) {
    static const uint8_t not_utf8[] = {0xFF, 0xFE};
    (void)input;
    (void)input_length;
    return emit(not_utf8, sizeof not_utf8, 0, output, output_length);
}

int32_t echo_where(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length) {
    uint64_t address = (uint64_t)(uintptr_t)input;
    (void)input_length;
    return emit((const uint8_t *)&address, sizeof address, 0, output, output_length);
}

int32_t echo_released(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length) {
    uint64_t count = released_count;
    (void)input;
    (void)input_length;
    return emit((const uint8_t *)&count, sizeof count, 0, output, output_length);
}

void echo_release(uint8_t *buffer) {
    if (buffer != NULL) {
        released_count++;
    }
    free(buffer);
}
