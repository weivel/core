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

static uint64_t *unload_counter = NULL;

static int definition_context;

static int32_t definition_echo(void *context, const uint8_t *input, size_t input_length, uint8_t **output,
                               size_t *output_length) {
    if (context != &definition_context) {
        return echo_fail(input, input_length, output, output_length);
    }
    return echo_echo(input, input_length, output, output_length);
}

static void definition_release(void *context, uint8_t *buffer) {
    (void)context;
    echo_release(buffer);
}

static void definition_count_unloads(void *context, uint64_t *unloads) {
    (void)context;
    unload_counter = unloads;
}

static const echo_definition definition = {
    sizeof(echo_definition), &definition_context, definition_echo, definition_release, definition_count_unloads};

static const uint32_t one_byte_short_definition = (uint32_t)(2 * sizeof(void *) - 1);

const void *echo_service_definition(void) {
    return &definition;
}

const void *echo_null_definition(void) {
    return NULL;
}

const void *echo_one_byte_short_definition(void) {
    return &one_byte_short_definition;
}

static void count_unload(void) {
    if (unload_counter != NULL) {
        (*unload_counter)++;
    }
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        count_unload();
    }
    return TRUE;
}
#else
__attribute__((destructor)) static void count_unload_on_close(void) {
    count_unload();
}
#endif
