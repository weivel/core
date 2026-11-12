#include <weivel/core.h>

#include "check.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#define ITERATIONS 20000

typedef struct hammer {
    weivel_core *core;
    unsigned queued;
    unsigned drained;
    unsigned damaged;
} hammer;

static int drain_one(hammer *worker) {
    weivel_event event = WEIVEL_INIT(weivel_event);
    if (weivel_next_event(worker->core, &event, NULL) != WEIVEL_STATUS_OK) {
        return 0;
    }
    if (event.kind != WEIVEL_EVENT_REFUSAL || strncmp(event.data.refusal.detail, "weivel_next_deadline", 20) != 0) {
        worker->damaged++;
    }
    weivel_event_release(&event);
    worker->drained++;
    return 1;
}

static void hammer_core(hammer *worker) {
    for (int index = 0; index < ITERATIONS; index++) {
        if (weivel_next_deadline(worker->core, NULL, NULL) == WEIVEL_STATUS_MALFORMED) {
            worker->queued++;
        }
        weivel_advance(worker->core, index, NULL);
        if (index % 3 != 0) {
            drain_one(worker);
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI run_hammer(LPVOID argument) {
    hammer_core(argument);
    return 0;
}
#else
static void *run_hammer(void *argument) {
    hammer_core(argument);
    return NULL;
}
#endif

static void run_two(hammer *first, hammer *second) {
#if defined(_WIN32)
    HANDLE thread = CreateThread(NULL, 0, run_hammer, first, 0, NULL);
    CHECK(thread != NULL);
    hammer_core(second);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, run_hammer, first) == 0);
    hammer_core(second);
    pthread_join(thread, NULL);
#endif
}

static void two_threads_hammering_one_core_lose_and_damage_nothing(void) {
    weivel_core *core = NULL;
    CHECK(weivel_core_create(NULL, &core, NULL) == WEIVEL_STATUS_OK);
    hammer first = {.core = core};
    hammer second = {.core = core};
    hammer rest = {.core = core};

    run_two(&first, &second);
    for (int index = 0; index <= 2 * ITERATIONS && drain_one(&rest); index++) {
    }

    CHECK(first.queued == ITERATIONS);
    CHECK(second.queued == ITERATIONS);
    CHECK(first.drained + second.drained + rest.drained == 2 * ITERATIONS);
    CHECK(first.damaged + second.damaged + rest.damaged == 0);
    weivel_core_destroy(core);
}

int main(void) {
    two_threads_hammering_one_core_lose_and_damage_nothing();
    return check_summary("concurrency_test");
}
