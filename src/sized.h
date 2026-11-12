#ifndef WEIVEL_SIZED_H
#define WEIVEL_SIZED_H

#include <stddef.h>
#include <stdint.h>

/**
 * Reads a caller's size-prefixed struct into the core's own full-size copy: the bytes the caller's `size` covers
 * are copied and every byte beyond is zero, which each field reads as its default.
 */
void weivel_sized_read(void *own, size_t own_size, const void *callers);

/**
 * Writes the core's full-size struct into a caller's, touching only the bytes the caller's `size` covers and
 * leaving `size` itself as the caller set it. Bytes the caller has beyond the core's struct are zeroed.
 */
void weivel_sized_write(void *callers, const void *own, size_t own_size);

/** The `size` a size-prefixed struct declares. */
uint32_t weivel_sized_size(const void *sized);

#endif
