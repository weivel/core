#include "sized.h"

#include <string.h>

uint32_t weivel_sized_size(const void *sized) {
    uint32_t size;
    memcpy(&size, sized, sizeof size);
    return size;
}

void weivel_sized_read(void *own, size_t own_size, const void *callers) {
    size_t covered = weivel_sized_size(callers);
    if (covered > own_size) {
        covered = own_size;
    }
    memset(own, 0, own_size);
    memcpy(own, callers, covered);
}

void weivel_sized_write(void *callers, const void *own, size_t own_size) {
    size_t declared = weivel_sized_size(callers);
    size_t covered = declared < own_size ? declared : own_size;
    unsigned char *destination = callers;
    const unsigned char *source = own;
    if (covered > sizeof(uint32_t)) {
        memcpy(destination + sizeof(uint32_t), source + sizeof(uint32_t), covered - sizeof(uint32_t));
    }
    if (declared > own_size) {
        memset(destination + own_size, 0, declared - own_size);
    }
}
