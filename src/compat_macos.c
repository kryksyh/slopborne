#include "compat_macos.h"
#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int remaining(clockid_t clock, const struct timespec *end, struct timespec *left) {
    if (end->tv_nsec < 0 || end->tv_nsec >= 1000000000) return EINVAL;
    struct timespec now;
    if (clock_gettime(clock, &now)) return errno;
    left->tv_sec = end->tv_sec - now.tv_sec;
    left->tv_nsec = end->tv_nsec - now.tv_nsec;
    if (left->tv_nsec < 0) { --left->tv_sec; left->tv_nsec += 1000000000; }
    return left->tv_sec < 0 || (!left->tv_sec && !left->tv_nsec) ? ETIMEDOUT : 0;
}

/* Darwin has no timed mutex/rwlock API. Poll with a bounded sleep and honor the
 * POSIX absolute realtime deadline, including an immediately available lock. */
#define TIMED_LOCK(name, type, attempt) \
int name(type *lock, const struct timespec *end) { \
    for (;;) { \
        int e = attempt(lock); \
        if (e != EBUSY) return e; \
        struct timespec nap; \
        e = remaining(CLOCK_REALTIME, end, &nap); \
        if (e) return e; \
        if (nap.tv_sec || nap.tv_nsec > 1000000) nap = (struct timespec){0, 1000000}; \
        while (nanosleep(&nap, &nap) && errno == EINTR) {} \
    } \
}
TIMED_LOCK(bb_macos_mutex_timedlock, pthread_mutex_t, pthread_mutex_trylock)
TIMED_LOCK(bb_macos_rwlock_timedrdlock, pthread_rwlock_t, pthread_rwlock_tryrdlock)
TIMED_LOCK(bb_macos_rwlock_timedwrlock, pthread_rwlock_t, pthread_rwlock_trywrlock)

int bb_macos_cond_timedwait_monotonic(pthread_cond_t *cond, pthread_mutex_t *lock,
                                    const struct timespec *end) {
    struct timespec left;
    int e = remaining(CLOCK_MONOTONIC, end, &left);
    return e ? e : pthread_cond_timedwait_relative_np(cond, lock, &left);
}

/* VM_FLAGS_FIXED fails on collisions; unlike MAP_FIXED it never overwrites a
 * host mapping. mmap hints alone are not sufficient for PS4 40-bit pointers. */
void *bb_macos_map_fixed(uintptr_t address, size_t size, int prot) {
    mach_vm_address_t target = address;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &target, size, VM_FLAGS_FIXED);
    if (kr != KERN_SUCCESS) { errno = kr == KERN_NO_SPACE ? EEXIST : ENOMEM; return MAP_FAILED; }
    if (mprotect((void *)target, size, prot)) {
        int saved = errno;
        mach_vm_deallocate(mach_task_self(), target, size);
        errno = saved;
        return MAP_FAILED;
    }
    return (void *)target;
}

/* Unlink immediately: the sparse shared object lives only as long as its fd. */
int bb_macos_memory_fd(void) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        char name[32];
        snprintf(name, sizeof(name), "/bb-%x-%08x", (unsigned)getpid(), arc4random());
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) { if (errno == EEXIST) continue; return -1; }
        shm_unlink(name);
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
        return fd;
    }
    errno = EEXIST;
    return -1;
}
#endif
