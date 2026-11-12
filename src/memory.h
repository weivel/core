#ifndef WEIVEL_MEMORY_H
#define WEIVEL_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * The one allocator every file of the core allocates through. A build for the tests (WEIVEL_TESTING) can make any
 * single allocation fail on demand; every other build compiles each call down to the C library's own, so the seam
 * costs nothing where it is not armed and is absent from the library a host links. Freeing goes to `free` directly:
 * it never fails, so it needs no seam.
 */
#ifdef WEIVEL_TESTING

void *weivel_memory_allocate(size_t bytes);
void *weivel_memory_allocate_zeroed(size_t count, size_t bytes);
void *weivel_memory_reallocate(void *block, size_t bytes);

/**
 * Arms the seam so that the `nth` allocation from now fails, 1 being the very next, and every other succeeds; 0
 * disarms it. Counting happens only while armed, so a disarmed seam touches no shared state from any thread.
 */
void weivel_memory_fail_at(uint64_t nth);

/** Whether the armed failure has happened since the seam was last armed. */
bool weivel_memory_failed(void);

#else

static inline void *weivel_memory_allocate(size_t bytes) {
    return malloc(bytes);
}

static inline void *weivel_memory_allocate_zeroed(size_t count, size_t bytes) {
    return calloc(count, bytes);
}

static inline void *weivel_memory_reallocate(void *block, size_t bytes) {
    return realloc(block, bytes);
}

#endif

#endif
