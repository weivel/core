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

/**
 * What is wrong with a manifest's shape, as the two texts of its refusal, each built at its length and NULL when
 * memory ran out, owned until weivel_manifest_problem_free.
 */
typedef struct weivel_manifest_problem {
    char *detail;
    char *fix;
} weivel_manifest_problem;

void weivel_manifest_problem_free(weivel_manifest_problem *problem);

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
 * WEIVEL_STATUS_REFUSED with both texts of `problem` filled when its shape is wrong, WEIVEL_STATUS_OUT_OF_MEMORY,
 * with nothing to free, when memory runs out copying it or describing its problem, or WEIVEL_STATUS_OK with `owned`
 * to be passed to weivel_manifest_free.
 */
weivel_status weivel_manifest_copy(const weivel_manifest *callers, weivel_owned_manifest *owned,
                                   weivel_manifest_problem *problem);

void weivel_manifest_free(weivel_owned_manifest *owned);

/** Whether a list of strings can be read: items present when counted, and no item NULL. */
bool weivel_strings_readable(const weivel_strings *strings);

/** Copies a readable list of strings into one allocation, `*storage`; false when memory runs out. */
bool weivel_strings_copy(const weivel_strings *source, weivel_strings *copy, void **storage);

/**
 * Whether a caller's list of size-prefixed elements can be read: items present when counted, the first element's
 * size at least `first_size`, and every element declaring the same.
 */
bool weivel_elements_readable(const void *items, uint32_t count, size_t first_size);

/** Reads element `index` of a readable list into the core's own full-size struct, defaulting what it lacks. */
void weivel_element_read(const void *items, uint32_t count, uint32_t index, void *own, size_t own_size);

/** Whether a caller's numbers can be read, every name non-empty and none twice. */
bool weivel_numbers_readable(const weivel_numbers *numbers);

#endif
