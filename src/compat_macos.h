#ifndef BB_COMPAT_MACOS_H
#define BB_COMPAT_MACOS_H
#ifdef __APPLE__
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>

/* Darwin's stat timestamps use the BSD names. */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER

int bb_macos_mutex_timedlock(pthread_mutex_t *, const struct timespec *);
int bb_macos_rwlock_timedrdlock(pthread_rwlock_t *, const struct timespec *);
int bb_macos_rwlock_timedwrlock(pthread_rwlock_t *, const struct timespec *);
int bb_macos_cond_timedwait_monotonic(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
void *bb_macos_map_fixed(uintptr_t, size_t, int);
int bb_macos_memory_fd(void);
#define pthread_mutex_timedlock bb_macos_mutex_timedlock
#define pthread_rwlock_timedrdlock bb_macos_rwlock_timedrdlock
#define pthread_rwlock_timedwrlock bb_macos_rwlock_timedwrlock
#endif
#endif
