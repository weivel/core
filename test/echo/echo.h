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

/** Frees a body any call answered; NULL is a no-op. */
ECHO_API void echo_release(uint8_t *buffer);

#ifdef __cplusplus
}
#endif

#endif
