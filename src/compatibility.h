#ifndef WEIVEL_COMPATIBILITY_H
#define WEIVEL_COMPATIBILITY_H

#include <weivel/core.h>

/** What a host is, for compatibility: the applications it hosts (empty for every one) and the api it implements. */
typedef struct weivel_host {
    weivel_strings applications;
    uint32_t api;
} weivel_host;

/**
 * Fills `answer`, a full-size zeroed struct, with the first axis on which `host` refuses `manifest`, a manifest the
 * core owns, or with `supported` when none does. The refusal's texts go into `answer->storage`.
 */
weivel_status weivel_compatibility_decide(const weivel_host *host, const weivel_manifest *manifest,
                                          weivel_support *answer);

#endif
