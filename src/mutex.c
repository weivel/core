#include "mutex.h"

#include <stdlib.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct weivel_mutex {
    SRWLOCK lock;
};

weivel_mutex *weivel_mutex_create(void) {
    weivel_mutex *mutex = malloc(sizeof *mutex);
    if (mutex != NULL) {
        InitializeSRWLock(&mutex->lock);
    }
    return mutex;
}

void weivel_mutex_destroy(weivel_mutex *mutex) {
    free(mutex);
}

void weivel_mutex_lock(weivel_mutex *mutex) {
    AcquireSRWLockExclusive(&mutex->lock);
}

void weivel_mutex_unlock(weivel_mutex *mutex) {
    ReleaseSRWLockExclusive(&mutex->lock);
}

#else

#include <pthread.h>

struct weivel_mutex {
    pthread_mutex_t lock;
};

weivel_mutex *weivel_mutex_create(void) {
    weivel_mutex *mutex = malloc(sizeof *mutex);
    if (mutex != NULL && pthread_mutex_init(&mutex->lock, NULL) != 0) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

void weivel_mutex_destroy(weivel_mutex *mutex) {
    if (mutex != NULL) {
        pthread_mutex_destroy(&mutex->lock);
        free(mutex);
    }
}

void weivel_mutex_lock(weivel_mutex *mutex) {
    pthread_mutex_lock(&mutex->lock);
}

void weivel_mutex_unlock(weivel_mutex *mutex) {
    pthread_mutex_unlock(&mutex->lock);
}

#endif
