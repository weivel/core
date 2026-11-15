#ifndef WEIVEL_TEST_ECHO_H
#define WEIVEL_TEST_ECHO_H

/**
 * @file
 * A neutral C library a language layer's tests bind as a host service, built beside the core so they need no other
 * repo. It is a test target only: never linked into the core and exported by nothing the core ships.
 *
 * Every call answers a status and a body the caller frees with echo_release: 0 with the body asked for, 1 with an
 * English text saying why it refused, 2 with no body when memory ran out.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(ECHO_BUILDING)
#define ECHO_API __declspec(dllexport)
#else
#define ECHO_API __declspec(dllimport)
#endif
#else
#define ECHO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Answers 0 and a copy of the input's bytes. */
ECHO_API int32_t echo_echo(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length);

/** Refuses whatever it is given: answers 1 and a text saying so. */
ECHO_API int32_t echo_fail(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length);

/** Answers 0 and two bytes that are not UTF-8, so a binding can prove it refuses to read them as text. */
ECHO_API int32_t echo_garbage(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length);

/**
 * Answers 0 and 8 bytes holding, in the platform's byte order, the address of the input it was handed, so a binding
 * can prove whether its caller's bytes reached the library without a copy.
 */
ECHO_API int32_t echo_where(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length);

/**
 * Answers 0 and 8 bytes holding, in the platform's byte order, how many non-NULL bodies echo_release has freed so
 * far, so a binding can prove it releases every body it was answered. A read counts itself: the body it answers is
 * counted once released, so two reads in a row, each released, differ by one.
 *
 * @implNote The count is kept without synchronisation and stays so, so it may be read only while no other thread
 * releases: a test that releases from several threads must not assert on it.
 */
ECHO_API int32_t echo_released(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length);

/** Frees a body any call answered; NULL is a no-op. */
ECHO_API void echo_release(uint8_t *buffer);

/**
 * The struct a definition-kind service answers: its `size`, then its operations, each taking `context` first.
 * Fields are only ever appended.
 */
typedef struct echo_definition {
    uint32_t size;
    void *context;
    /** As echo_echo, but answers 1 and a text saying so when handed any context but the definition's own. */
    int32_t (*echo)(void *context, const uint8_t *input, size_t input_length, uint8_t **output,
                    size_t *output_length);
    /** As echo_release, and counted by echo_released alike. */
    void (*release)(void *context, uint8_t *buffer);
    /**
     * Hands the library a counter it adds one to when its binary is closed; NULL stops counting. A binary keeps
     * nothing once closed, so the count lives in the caller's memory, which must outlive the binary (process exit
     * closes it too), and a binary loaded again counts nothing until it is handed a counter.
     */
    void (*count_unloads)(void *context, uint64_t *unloads);
} echo_definition;

/** Answers one static echo_definition, the same on every call, living until the library is unloaded. */
ECHO_API const void *echo_service_definition(void);

/** A definition that answers NULL, so a host can prove it refuses the load. */
ECHO_API const void *echo_null_definition(void);

/** A definition whose struct's `size` is 4, below a size and one pointer, so a host can prove it refuses the load. */
ECHO_API const void *echo_short_definition(void);

#ifdef __cplusplus
}
#endif

#endif
