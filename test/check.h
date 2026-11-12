#ifndef WEIVEL_TEST_CHECK_H
#define WEIVEL_TEST_CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            check_failures++;                                                             \
        }                                                                                 \
    } while (0)

static inline int check_text(const char *actual, const char *expected, const char *label) {
    if (actual != NULL && expected != NULL && strcmp(actual, expected) == 0) {
        return 1;
    }
    fprintf(stderr, "%s: expected \"%s\", got \"%s\"\n", label, expected == NULL ? "(null)" : expected,
            actual == NULL ? "(null)" : actual);
    check_failures++;
    return 0;
}

static inline int check_summary(const char *suite) {
    if (check_failures == 0) {
        printf("%s: all checks passed\n", suite);
        return 0;
    }
    fprintf(stderr, "%s: %d checks failed\n", suite, check_failures);
    return 1;
}

#endif
