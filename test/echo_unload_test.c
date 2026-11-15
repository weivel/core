#include <string.h>

#include "check.h"
#include "echo/echo.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef const void *(*definition_symbol)(void);

typedef struct loaded_library {
#if defined(_WIN32)
    HMODULE module;
#else
    void *module;
#endif
} loaded_library;

static loaded_library open_library(const char *path) {
    loaded_library library;
#if defined(_WIN32)
    library.module = LoadLibraryA(path);
#else
    library.module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    return library;
}

/** @implNote dlsym answers an object pointer, which ISO C forbids casting to a function pointer, so it is copied. */
static definition_symbol find_definition(loaded_library library) {
    definition_symbol symbol = NULL;
#if defined(_WIN32)
    FARPROC found = GetProcAddress(library.module, "echo_service_definition");
    memcpy(&symbol, &found, sizeof symbol);
#else
    void *found = dlsym(library.module, "echo_service_definition");
    memcpy(&symbol, &found, sizeof symbol);
#endif
    return symbol;
}

static void close_library(loaded_library library) {
#if defined(_WIN32)
    CHECK(FreeLibrary(library.module) != 0);
#else
    CHECK(dlclose(library.module) == 0);
#endif
}

static uint64_t unloads = 0;

static const echo_definition *load_definition(const char *path, loaded_library *library) {
    *library = open_library(path);
    CHECK(library->module != NULL);
    if (library->module == NULL) {
        return NULL;
    }
    definition_symbol symbol = find_definition(*library);
    CHECK(symbol != NULL);
    return symbol == NULL ? NULL : (const echo_definition *)symbol();
}

static void a_definition_counts_its_own_unload(const char *path) {
    loaded_library library;
    const echo_definition *definition = load_definition(path, &library);
    if (definition == NULL) {
        return;
    }
    uint64_t before = unloads;
    definition->count_unloads(definition->context, &unloads);
    uint8_t *bytes = NULL;
    size_t length = 0;
    CHECK(definition->echo(definition->context, (const uint8_t *)"open", 4, &bytes, &length) == 0);
    definition->release(definition->context, bytes);
    CHECK(unloads == before);
    close_library(library);
    CHECK(unloads == before + 1);
}

static void a_binary_loaded_again_counts_nothing_until_handed_a_counter(const char *path) {
    loaded_library library;
    const echo_definition *definition = load_definition(path, &library);
    if (definition == NULL) {
        return;
    }
    uint64_t before = unloads;
    close_library(library);
    CHECK(unloads == before);
}

static void a_counter_taken_back_counts_nothing(const char *path) {
    loaded_library library;
    const echo_definition *definition = load_definition(path, &library);
    if (definition == NULL) {
        return;
    }
    uint64_t before = unloads;
    definition->count_unloads(definition->context, &unloads);
    definition->count_unloads(definition->context, NULL);
    close_library(library);
    CHECK(unloads == before);
}

int main(int argument_count, char **arguments) {
    CHECK(argument_count == 2);
    if (argument_count != 2) {
        return check_summary("echo_unload_test");
    }
    a_definition_counts_its_own_unload(arguments[1]);
    a_binary_loaded_again_counts_nothing_until_handed_a_counter(arguments[1]);
    a_definition_counts_its_own_unload(arguments[1]);
    a_counter_taken_back_counts_nothing(arguments[1]);
    return check_summary("echo_unload_test");
}
