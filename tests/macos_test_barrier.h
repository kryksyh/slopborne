/* pthread barriers are absent on Darwin; test-only reusable barrier. */
#ifndef BB_MACOS_TEST_BARRIER_H
#define BB_MACOS_TEST_BARRIER_H
#ifdef __APPLE__
#include <pthread.h>
#include <errno.h>
#define PTHREAD_BARRIER_SERIAL_THREAD 1
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned count, waiting, generation;
} pthread_barrier_t;
static int pthread_barrier_init(pthread_barrier_t *b, const void *attr, unsigned count) {
    (void)attr;
    if (!count) return EINVAL;
    *b=(pthread_barrier_t){.count=count};
    int e=pthread_mutex_init(&b->mutex,NULL);
    if (e) return e;
    e=pthread_cond_init(&b->cond,NULL);
    if (e) pthread_mutex_destroy(&b->mutex);
    return e;
}
static int pthread_barrier_wait(pthread_barrier_t *b) {
    pthread_mutex_lock(&b->mutex);
    unsigned generation=b->generation;
    int result=0;
    if (++b->waiting==b->count) {
        b->waiting=0;
        ++b->generation;
        pthread_cond_broadcast(&b->cond);
        result=PTHREAD_BARRIER_SERIAL_THREAD;
    } else {
        while (generation==b->generation) pthread_cond_wait(&b->cond,&b->mutex);
    }
    pthread_mutex_unlock(&b->mutex);
    return result;
}
static int pthread_barrier_destroy(pthread_barrier_t *b) {
    int e=pthread_cond_destroy(&b->cond);
    return e ? e : pthread_mutex_destroy(&b->mutex);
}
#endif
#endif
