#include "memory.h"

static uint64_t countdown;
static bool failed;

/** Whether this allocation is the one armed to fail, counting it when the seam is armed. */
static bool fails_now(void) {
    if (countdown == 0) {
        return false;
    }
    countdown--;
    if (countdown > 0) {
        return false;
    }
    failed = true;
    return true;
}

void *weivel_memory_allocate(size_t bytes) {
    return fails_now() ? NULL : malloc(bytes);
}

void *weivel_memory_allocate_zeroed(size_t count, size_t bytes) {
    return fails_now() ? NULL : calloc(count, bytes);
}

void *weivel_memory_reallocate(void *block, size_t bytes) {
    return fails_now() ? NULL : realloc(block, bytes);
}

void weivel_memory_fail_at(uint64_t nth) {
    countdown = nth;
    failed = false;
}

bool weivel_memory_failed(void) {
    return failed;
}
