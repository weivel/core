#include "compatibility.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events.h"

static char *with_texts(const char *format, const char *first, const char *second) {
    int length = snprintf(NULL, 0, format, first, second);
    char *text = length < 0 ? NULL : malloc((size_t)length + 1);
    if (text != NULL) {
        snprintf(text, (size_t)length + 1, format, first, second);
    }
    return text;
}

static char *joined(const weivel_strings *strings) {
    const char *separator = ", ";
    size_t length = 1;
    for (uint32_t index = 0; index < strings->count; index++) {
        length += strlen(strings->items[index]) + (index > 0 ? strlen(separator) : 0);
    }
    char *text = malloc(length);
    if (text == NULL) {
        return NULL;
    }
    char *cursor = text;
    for (uint32_t index = 0; index < strings->count; index++) {
        if (index > 0) {
            memcpy(cursor, separator, strlen(separator));
            cursor += strlen(separator);
        }
        size_t item_length = strlen(strings->items[index]);
        memcpy(cursor, strings->items[index], item_length);
        cursor += item_length;
    }
    *cursor = '\0';
    return text;
}

static bool hosts_any_target(const weivel_host *host, const weivel_strings *targets) {
    for (uint32_t target = 0; target < targets->count; target++) {
        for (uint32_t application = 0; application < host->applications.count; application++) {
            if (strcmp(targets->items[target], host->applications.items[application]) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool application_refuses(const weivel_host *host, const weivel_manifest *manifest) {
    return manifest->targets.count > 0 && host->applications.count > 0 &&
           !hosts_any_target(host, &manifest->targets);
}

static weivel_status refuse(weivel_support *answer, uint32_t axis, const char *detail, const char *fix) {
    weivel_status status = WEIVEL_STATUS_OUT_OF_MEMORY;
    if (detail != NULL && fix != NULL) {
        const char *texts[2] = {detail, fix};
        const char *copies[2];
        answer->storage = weivel_events_copy_texts(2, texts, copies);
        if (answer->storage != NULL) {
            answer->supported = 0;
            answer->axis = axis;
            answer->refusal.code = WEIVEL_REFUSAL_INCOMPATIBLE;
            answer->refusal.detail = copies[0];
            answer->refusal.fix = copies[1];
            status = WEIVEL_STATUS_OK;
        }
    }
    return status;
}

static weivel_status refuse_application(const weivel_host *host, const weivel_manifest *manifest,
                                        weivel_support *answer) {
    char *targets = joined(&manifest->targets);
    char *applications = joined(&host->applications);
    char *detail = NULL;
    if (targets != NULL && applications != NULL) {
        detail = with_texts("is built for %s, and this host hosts %s", targets, applications);
    }
    char *fix = with_texts("install it in %s, or add \"%s\" to \"targets\" in its manifest",
                           manifest->targets.items[0], host->applications.items[0]);
    weivel_status status = refuse(answer, WEIVEL_AXIS_APPLICATION, detail, fix);
    free(targets);
    free(applications);
    free(detail);
    free(fix);
    return status;
}

static weivel_status refuse_api(const weivel_host *host, const weivel_manifest *manifest, weivel_support *answer) {
    char detail[64];
    char fix[96];
    snprintf(detail, sizeof detail, "needs api %u, this host has api %u", (unsigned)manifest->api, (unsigned)host->api);
    snprintf(fix, sizeof fix, "update the app to a version that implements api %u or later", (unsigned)manifest->api);
    return refuse(answer, WEIVEL_AXIS_API, detail, fix);
}

weivel_status weivel_compatibility_decide(const weivel_host *host, const weivel_manifest *manifest,
                                          weivel_support *answer) {
    if (application_refuses(host, manifest)) {
        return refuse_application(host, manifest, answer);
    }
    if (manifest->api > host->api) {
        return refuse_api(host, manifest, answer);
    }
    answer->supported = 1;
    answer->axis = WEIVEL_AXIS_NONE;
    return WEIVEL_STATUS_OK;
}
