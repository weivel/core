#ifndef WEIVEL_MANIFEST_H
#define WEIVEL_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <weivel/core.h>

/** The smallest `size` each manifest struct accepts: its shape when first published, fixed forever. */
#define WEIVEL_MANIFEST_FIRST_SIZE offsetof(weivel_manifest, permissions)
#define WEIVEL_SERVICE_OFFER_FIRST_SIZE (offsetof(weivel_service_offer, id) + sizeof(const char *))
#define WEIVEL_SERVICE_WANT_FIRST_SIZE (offsetof(weivel_service_want, degradable) + sizeof(uint32_t))
#define WEIVEL_NUMBER_FIRST_SIZE (offsetof(weivel_number, value) + sizeof(double))

/** What is wrong with a manifest's shape, as the two texts of its refusal. */
typedef struct weivel_manifest_problem {
    char detail[384];
    char fix[256];
} weivel_manifest_problem;

/**
 * A manifest the core owns. Every pointer in `value` points into `storage`, every struct in it is at today's size,
 * an empty list has NULL items, `optional` and `degradable` hold WEIVEL_CHOICE_YES or WEIVEL_CHOICE_NO, never
 * the default, and `identity` is never NULL, so nothing reading it resolves a default again.
 */
typedef struct weivel_owned_manifest {
    weivel_manifest value;
    void *storage;
} weivel_owned_manifest;

/**
 * Checks a caller's manifest, whose size the caller has checked against WEIVEL_MANIFEST_FIRST_SIZE, and copies it.
 * WEIVEL_STATUS_REFUSED with `problem` filled when its shape is wrong, WEIVEL_STATUS_OUT_OF_MEMORY, or
 * WEIVEL_STATUS_OK with `owned` to be passed to weivel_manifest_free.
 */
weivel_status weivel_manifest_copy(const weivel_manifest *callers, weivel_owned_manifest *owned,
                                   weivel_manifest_problem *problem);

void weivel_manifest_free(weivel_owned_manifest *owned);

/** Whether a list of strings can be read: items present when counted, and no item NULL. */
bool weivel_strings_readable(const weivel_strings *strings);

/** Copies a readable list of strings into one allocation, `*storage`; false when memory runs out. */
bool weivel_strings_copy(const weivel_strings *source, weivel_strings *copy, void **storage);

#endif
