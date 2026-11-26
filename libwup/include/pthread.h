#pragma once
/* <pthread.h> on the pad: the usual subset (threads, mutexes, condition variables, once, keys), on libwup's threads
 * and semaphores (kernel.c), which are the wup-homebrew kernel's. Enough for portable libraries.
 * Differences from a desktop: at most WUP_THREADS threads; threads of one priority do not take turns by themselves
 * (one runs until it waits or sleeps); errno is shared by all threads; no cancellation, no signals. */
#include <stddef.h>
#include <time.h>
#include <sched.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned pthread_t;                           /* 0: the main thread */
typedef struct { unsigned stack_size; int detached; } pthread_attr_t;
typedef struct { int sem; unsigned owner; int count, type; } pthread_mutex_t;
typedef struct { int type; } pthread_mutexattr_t;
typedef struct { int sem, waiters; } pthread_cond_t;
typedef struct { int unused; } pthread_condattr_t;
typedef struct { int state; pthread_mutex_t lock; } pthread_once_t;
typedef unsigned pthread_key_t;

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0, PTHREAD_MUTEX_NORMAL }
#define PTHREAD_COND_INITIALIZER  { 0, 0 }
#define PTHREAD_ONCE_INIT         { 0, PTHREAD_MUTEX_INITIALIZER }
#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1
#define PTHREAD_STACK_MIN        2048
#define PTHREAD_KEYS_MAX         16

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t thread, void **ret);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
void pthread_exit(void *ret) __attribute__((noreturn));
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);    /* the default is 8 KB: memory is tight */
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size);
int pthread_attr_setdetachstate(pthread_attr_t *a, int state);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until);   /* CLOCK_REALTIME */
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_condattr_init(pthread_condattr_t *a);
int pthread_condattr_destroy(pthread_condattr_t *a);

int pthread_once(pthread_once_t *once, void (*fn)(void));
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
#ifdef __cplusplus
}
#endif
