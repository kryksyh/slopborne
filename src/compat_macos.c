#include "compat_macos.h"
#ifdef __APPLE__
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <objc/objc.h>
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
/* [[NSProcessInfo processInfo] beginActivityWithOptions:reason:] through the Objective-C runtime:
 * UserInitiated | LatencyCritical turns off timer coalescing and App Nap, which delay the game's
 * short sleeps and the GPU threads' wakeups (on battery most). Kept for the whole process.
 * From PabloVSouza/bloodborne_mac e8fe937. */
void bb_macos_latency_critical(void) {
    typedef void *(*GetClass)(const char *);
    typedef SEL (*RegisterName)(const char *);
    typedef void *(*Send0)(void *, SEL);
    typedef void *(*SendString)(void *, SEL, const char *);
    typedef void *(*SendActivity)(void *, SEL, unsigned long long, void *);
    GetClass get_class=(GetClass)dlsym(RTLD_DEFAULT,"objc_getClass");
    RegisterName sel=(RegisterName)dlsym(RTLD_DEFAULT,"sel_registerName");
    void *send=dlsym(RTLD_DEFAULT,"objc_msgSend");
    if (!get_class || !sel || !send) return;
    void *process_class=get_class("NSProcessInfo"), *string_class=get_class("NSString");
    if (!process_class || !string_class) return;
    void *info=((Send0)send)(process_class,sel("processInfo"));
    void *reason=((SendString)send)(string_class,sel("stringWithUTF8String:"),"Bloodborne");
    const unsigned long long user_initiated=0x00FFFFFFULL, latency_critical=0xFF00000000ULL;
    void *activity=((SendActivity)send)(info,sel("beginActivityWithOptions:reason:"),
                                        user_initiated|latency_critical,reason);
    if (activity) ((Send0)send)(activity,sel("retain"));
}
#endif
