#ifndef WEIVEL_MUTEX_H
#define WEIVEL_MUTEX_H

/** A non-recursive lock. @implNote The only platform-specific code in the core lives behind this type. */
typedef struct weivel_mutex weivel_mutex;

weivel_mutex *weivel_mutex_create(void);
void weivel_mutex_destroy(weivel_mutex *mutex);
void weivel_mutex_lock(weivel_mutex *mutex);
void weivel_mutex_unlock(weivel_mutex *mutex);

#endif
