#ifndef WEIVEL_LIST_H
#define WEIVEL_LIST_H

#include <stdbool.h>
#include <stddef.h>

/**
 * A link of a circular doubly-linked list, embedded in the record it chains. A list's head is a link of its own
 * that no record contains; a link on no list points at itself, so removing it twice is harmless.
 */
typedef struct weivel_link {
    struct weivel_link *previous;
    struct weivel_link *next;
} weivel_link;

/** The record of `type` whose `member` is the link at `pointer`. */
#define WEIVEL_RECORD_OF(pointer, type, member) ((type *)(void *)((char *)(pointer) - offsetof(type, member)))

static inline void weivel_list_init(weivel_link *link) {
    link->previous = link;
    link->next = link;
}

static inline bool weivel_list_empty(const weivel_link *head) {
    return head->next == head;
}

static inline void weivel_list_append(weivel_link *head, weivel_link *link) {
    link->previous = head->previous;
    link->next = head;
    head->previous->next = link;
    head->previous = link;
}

static inline void weivel_list_remove(weivel_link *link) {
    link->previous->next = link->next;
    link->next->previous = link->previous;
    weivel_list_init(link);
}

#endif
