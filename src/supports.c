#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "manifest.h"
#include "sized.h"

static weivel_support empty_answer(void) {
    weivel_support answer;
    memset(&answer, 0, sizeof answer);
    answer.size = (uint32_t)sizeof answer;
    return answer;
}

static bool is_sized_answer(const weivel_support *answer) {
    return answer != NULL && answer->size >= WEIVEL_SUPPORT_FIRST_SIZE;
}

static weivel_status refuse_shape(weivel_support *answer, const weivel_manifest_problem *problem) {
    const char *texts[2] = {problem->detail, problem->fix};
    const char *copies[2];
    answer->storage = weivel_events_copy_texts(2, texts, copies);
    if (answer->storage == NULL) {
        return WEIVEL_STATUS_OUT_OF_MEMORY;
    }
    answer->refusal.code = WEIVEL_REFUSAL_MALFORMED_MANIFEST;
    answer->refusal.detail = copies[0];
    answer->refusal.fix = copies[1];
    return WEIVEL_STATUS_OK;
}

static weivel_status decide(const weivel_core *core, const weivel_manifest *manifest, weivel_support *answer) {
    weivel_owned_manifest owned;
    weivel_manifest_problem problem;
    weivel_status status = weivel_manifest_copy(manifest, &owned, &problem);
    if (status == WEIVEL_STATUS_REFUSED) {
        return refuse_shape(answer, &problem);
    }
    if (status == WEIVEL_STATUS_OK) {
        status = weivel_compatibility_decide(&core->host, &owned.value, answer);
        weivel_manifest_free(&owned);
    }
    return status;
}

static weivel_status answer_supports(weivel_core *core, const weivel_manifest *manifest, weivel_support *callers) {
    weivel_support answer = empty_answer();
    weivel_status status = decide(core, manifest, &answer);
    if (status != WEIVEL_STATUS_OK) {
        free(answer.storage);
        return status;
    }
    weivel_sized_write(callers, &answer, sizeof answer);
    return WEIVEL_STATUS_OK;
}

weivel_status weivel_supports(weivel_core *core, const weivel_manifest *manifest, weivel_support *answer,
                              uint32_t *pending_events) {
    if (core == NULL) {
        return WEIVEL_STATUS_MALFORMED;
    }
    weivel_mutex_lock(core->mutex);
    weivel_status status;
    if (!is_sized_answer(answer)) {
        status = weivel_core_refuse_malformed(
            core, "weivel_supports was given no answer struct, or one whose size is too small",
            "pass a weivel_support initialised with WEIVEL_INIT(weivel_support)");
    } else if (answer->storage != NULL) {
        status = weivel_core_refuse_malformed(core, "weivel_supports was given an answer still holding one",
                                              "pass the held answer to weivel_support_release first");
    } else if (manifest == NULL || weivel_sized_size(manifest) < WEIVEL_MANIFEST_FIRST_SIZE) {
        status = weivel_core_refuse_malformed(
            core, "weivel_supports was given no manifest, or one whose size is too small",
            "pass a weivel_manifest initialised with WEIVEL_INIT(weivel_manifest)");
    } else {
        status = answer_supports(core, manifest, answer);
    }
    weivel_core_report_pending(core, pending_events);
    weivel_mutex_unlock(core->mutex);
    return status;
}

weivel_status weivel_support_release(weivel_support *answer) {
    if (answer != NULL && answer->size == 0) {
        return WEIVEL_STATUS_OK;
    }
    if (!is_sized_answer(answer)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    free(answer->storage);
    weivel_support empty = empty_answer();
    weivel_sized_write(answer, &empty, sizeof empty);
    return WEIVEL_STATUS_OK;
}
