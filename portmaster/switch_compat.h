// switch_compat.h — minimal libnx surface so the ct_nx shim files (imports.c,
// libc_shim.c, jni_fake.c) build on Linux/glibc for the PortMaster port. Only the
// handful of libnx symbols those files actually use are provided. Included in
// place of <switch.h> when __SWITCH__ is not defined.
#ifndef CT_SWITCH_COMPAT_H
#define CT_SWITCH_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

// libnx integer aliases
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;

// result codes
typedef uint32_t Result;
#define R_SUCCEEDED(x) ((x) == 0)
#define R_FAILED(x)    ((x) != 0)

// Mutex — libnx Mutex is zero-init-safe; glibc PTHREAD_MUTEX_INITIALIZER is all
// zeros, so a static/zero-initialised pthread_mutex_t is equivalent.
typedef pthread_mutex_t Mutex;
static inline void mutexInit(Mutex *m)    { pthread_mutex_init(m, NULL); }
static inline void mutexLock(Mutex *m)    { pthread_mutex_lock(m); }
static inline void mutexUnlock(Mutex *m)  { pthread_mutex_unlock(m); }
static inline int  mutexTryLock(Mutex *m) { return pthread_mutex_trylock(m) == 0; }

// threads / sleep
#define CUR_THREAD_HANDLE 0u
static inline Result svcSleepThread(int64_t ns) {
  if (ns <= 0) { sched_yield(); return 0; }
  struct timespec ts = { (time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL) };
  nanosleep(&ts, NULL);
  return 0;
}
static inline Result svcGetThreadId(u64 *out, uint32_t handle) {
  (void)handle;
  if (out) *out = (u64)(uintptr_t)pthread_self();
  return 0;
}

// system tick — used for clock_gettime_fake; expose ns ticks at 1 GHz.
static inline u64 armGetSystemTickFreq(void) { return 1000000000ull; }
static inline u64 armGetSystemTick(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

// reader/writer lock -> pthread_rwlock (single unlock covers read & write).
typedef pthread_rwlock_t RwLock;
static inline void rwlockInit(RwLock *l)        { pthread_rwlock_init(l, NULL); }
static inline void rwlockReadLock(RwLock *l)    { pthread_rwlock_rdlock(l); }
static inline void rwlockWriteLock(RwLock *l)   { pthread_rwlock_wrlock(l); }
static inline void rwlockReadUnlock(RwLock *l)  { pthread_rwlock_unlock(l); }
static inline void rwlockWriteUnlock(RwLock *l) { pthread_rwlock_unlock(l); }
static inline bool rwlockIsWriteLockHeldByCurrentThread(RwLock *l) { (void)l; return false; }

// counting semaphore -> POSIX sem_t.
typedef sem_t Semaphore;
static inline void semaphoreInit(Semaphore *s, u64 v)  { sem_init(s, 0, (unsigned)v); }
static inline void semaphoreSignal(Semaphore *s)       { sem_post(s); }
static inline void semaphoreWait(Semaphore *s)         { sem_wait(s); }
static inline bool semaphoreTryWait(Semaphore *s)      { return sem_trywait(s) == 0; }

// newlib/bionic spell the errno accessor __errno; glibc uses __errno_location.
static inline int *__errno(void) { return __errno_location(); }

// software keyboard (IME): no on-screen keyboard on PortMaster handhelds. Stub so
// text entry yields no input for now (revisit with SDL_StartTextInput later).
typedef struct { int _unused; } SwkbdConfig;
static inline Result swkbdCreate(SwkbdConfig *c, int n)                       { (void)c; (void)n; return 1; }
static inline void   swkbdConfigMakePresetDefault(SwkbdConfig *c)            { (void)c; }
static inline void   swkbdConfigSetInitialText(SwkbdConfig *c, const char *s){ (void)c; (void)s; }
static inline void   swkbdConfigSetStringLenMax(SwkbdConfig *c, u32 n)       { (void)c; (void)n; }
static inline Result swkbdShow(SwkbdConfig *c, char *out, size_t len)        { (void)c; if (out && len) out[0] = 0; return 1; }
static inline void   swkbdClose(SwkbdConfig *c)                              { (void)c; }

#endif // CT_SWITCH_COMPAT_H
