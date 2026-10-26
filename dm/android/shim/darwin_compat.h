// The Darwin APIs the host uses, on Android (bionic). Included first in every host translation unit (-include), and by the shim headers that stand in for
// <mach/mach.h>, <dispatch/dispatch.h> and the rest. Each replacement keeps the Darwin call's meaning where the host relies on it (safe reads that fail
// instead of faulting, monotonic time in nanoseconds, semaphores with timeouts) and reports failure where Android has no counterpart (suspending another
// thread through Mach), so the callers' existing fallbacks run.
#pragma once
#ifndef __ANDROID__
#error darwin_compat.h is for Android builds
#endif
#define _GNU_SOURCE 1
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_MAC 0
#define TARGET_OS_VISION 0

// ---- Mach basics ----
typedef int kern_return_t;
typedef unsigned int mach_port_t, mach_msg_type_number_t, natural_t;
typedef mach_port_t thread_act_t, task_t, thread_t;
typedef thread_act_t *thread_act_array_t;
typedef uintptr_t vm_address_t, vm_offset_t, mach_vm_address_t;
typedef size_t vm_size_t, mach_vm_size_t;
typedef int vm_prot_t, vm_inherit_t, boolean_t;
typedef int *thread_state_t, *thread_info_t, *task_info_t, *vm_region_info_t;
#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define KERN_FAILURE 5
#define KERN_NOT_SUPPORTED 46
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_INHERIT_NONE 2
#define FALSE 0
#define TRUE 1
static inline mach_port_t mach_task_self(void) { return 0; }
static inline mach_port_t mach_thread_self(void) { return (mach_port_t)gettid(); }
static inline kern_return_t mach_port_deallocate(mach_port_t t, mach_port_t p) { (void)t; (void)p; return KERN_SUCCESS; }
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }
// Copy that fails (instead of faulting) when the source is not mapped readable: process_vm_readv on this process
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out);
kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags);
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size);
// Aliasing one range of memory at a second address. Android maps file-backed memory twice instead (memfd); until a caller is moved to that, it fails
kern_return_t vm_remap(mach_port_t task, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t src_task, vm_address_t from, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh);
// Thread suspension / inspection through Mach ports: none on Android (callers fall back to their signal paths or skip the diagnostic)
static inline kern_return_t thread_suspend(thread_act_t t) { (void)t; return KERN_NOT_SUPPORTED; }
static inline kern_return_t thread_resume(thread_act_t t) { (void)t; return KERN_NOT_SUPPORTED; }
static inline kern_return_t thread_get_state(thread_act_t t, int f, thread_state_t s, mach_msg_type_number_t *n) { (void)t; (void)f; (void)s; (void)n; return KERN_NOT_SUPPORTED; }
static inline kern_return_t thread_info(thread_act_t t, int f, thread_info_t i, mach_msg_type_number_t *n) { (void)t; (void)f; (void)i; (void)n; return KERN_NOT_SUPPORTED; }
static inline kern_return_t thread_policy_set(thread_act_t t, int f, int *p, mach_msg_type_number_t n) { (void)t; (void)f; (void)p; (void)n; return KERN_NOT_SUPPORTED; }
static inline kern_return_t task_threads(mach_port_t t, thread_act_array_t *a, mach_msg_type_number_t *n) { (void)t; *a = NULL; *n = 0; return KERN_NOT_SUPPORTED; }
static inline kern_return_t task_info(mach_port_t t, int f, task_info_t i, mach_msg_type_number_t *n) { (void)t; (void)f; (void)i; (void)n; return KERN_NOT_SUPPORTED; }

// ---- Mach info structures (diagnostics only: thread_info / thread_get_state / task_info above fail, so callers take their "not available" branch) ----
typedef struct { int seconds, microseconds; } time_value_t;
typedef struct { time_value_t user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(int)))
#define TH_STATE_RUNNING 1
typedef struct { uint64_t pth_user_time, pth_system_time; int pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(int)))
typedef struct { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; } arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(int)))
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
typedef struct { uint64_t virtual_size, resident_size, phys_footprint, resident_size_peak; } task_vm_info_data_t;
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))
typedef struct { struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups; } cpu_energy;
                 struct { uint64_t task_gpu_utilisation; } gpu_energy; uint64_t task_energy, task_ptime, task_pset_switches; } task_power_info_v2_data_t;
#define TASK_POWER_INFO_V2 26
#define TASK_POWER_INFO_V2_COUNT ((mach_msg_type_number_t)(sizeof(task_power_info_v2_data_t) / sizeof(int)))
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
typedef int *thread_policy_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(int)))
static inline kern_return_t thread_policy_get(thread_act_t t, int f, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *def) { (void)t; (void)f; (void)p; (void)n; if (def) *def = 1; return KERN_NOT_SUPPORTED; }
#define VM_REGION_BASIC_INFO_64 9
typedef struct { vm_prot_t protection, max_protection; vm_inherit_t inheritance; boolean_t shared, reserved; uint64_t offset; int behavior; unsigned short user_wired_count; } vm_region_basic_info_data_64_t;
#define MACH_PORT_NULL 0u
static inline kern_return_t vm_region_64(mach_port_t t, vm_address_t *a, vm_size_t *s, int f, vm_region_info_t i, mach_msg_type_number_t *n, mach_port_t *o) { (void)t; (void)a; (void)s; (void)f; (void)i; (void)n; (void)o; return KERN_NOT_SUPPORTED; }
#define mach_vm_region(t, a, s, f, i, n, o) vm_region_64((t), (vm_address_t *)(a), (vm_size_t *)(s), (f), (i), (n), (o))
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))

// ---- Time: Mach absolute time = CLOCK_MONOTONIC nanoseconds (timebase 1/1) ----
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t, *mach_timebase_info_t;
static inline uint64_t dm_mono_ns(clockid_t c) { struct timespec ts; clock_gettime(c, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static inline uint64_t mach_absolute_time(void) { return dm_mono_ns(CLOCK_MONOTONIC); }
static inline uint64_t mach_continuous_time(void) { return dm_mono_ns(CLOCK_BOOTTIME); }
static inline kern_return_t mach_timebase_info(mach_timebase_info_t t) { t->numer = t->denom = 1; return KERN_SUCCESS; }
static inline kern_return_t mach_wait_until(uint64_t deadline) {
    struct timespec ts = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    return KERN_SUCCESS;
}
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#define CLOCK_MONOTONIC_RAW_APPROX CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { return dm_mono_ns(c); }

// ---- Thread naming, stacks, timed waits, QoS ----
int dm_setname1(const char *name);   // Darwin's form names the calling thread; bionic limits names to 15 characters
#define DM_SETNAME_PICK(_1, _2, NAME, ...) NAME
#define pthread_setname_np(...) DM_SETNAME_PICK(__VA_ARGS__, dm_setname2, dm_setname1, _)(__VA_ARGS__)
static inline int dm_setname2(pthread_t t, const char *n) { char b[16]; strncpy(b, n, 15); b[15] = 0; return (pthread_setname_np)(t, b); }
void *pthread_get_stackaddr_np(pthread_t t);   // Darwin: the stack's highest address
size_t pthread_get_stacksize_np(pthread_t t);
int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel);
typedef enum {
    QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15,
    QOS_CLASS_UTILITY = 0x11, QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0x00,
} qos_class_t;
typedef void *pthread_override_t;
int pthread_set_qos_class_self_np(qos_class_t q, int rel);   // mapped to a nice value
int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel);
static inline pthread_override_t pthread_override_qos_class_start_np(pthread_t t, qos_class_t q, int r) { (void)t; (void)q; (void)r; return NULL; }
static inline int pthread_override_qos_class_end_np(pthread_override_t o) { (void)o; return 0; }
static inline int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int r) { (void)a; (void)q; (void)r; return 0; }
static inline qos_class_t qos_class_self(void) { return QOS_CLASS_UNSPECIFIED; }

// ---- libdispatch: semaphores only (the host's other dispatch uses are behind __APPLE__) ----
typedef struct dm_dsema *dispatch_semaphore_t;
typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW 0ull
#define DISPATCH_TIME_FOREVER (~0ull)
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
#define USEC_PER_SEC 1000000ull
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta);   // absolute CLOCK_MONOTONIC nanoseconds
dispatch_semaphore_t dispatch_semaphore_create(long value);
long dispatch_semaphore_signal(dispatch_semaphore_t s);
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t t);   // 0 = taken, nonzero = timed out
void dispatch_release(void *o);

// ---- Memory, caches, system queries ----
static inline size_t malloc_size(const void *p) { return malloc_usable_size((void *)p); }
static inline void sys_icache_invalidate(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }
static inline void sys_dcache_flush(void *p, size_t n) { (void)p; (void)n; }
static inline int getdtablesize(void) { return (int)sysconf(_SC_OPEN_MAX); }
int sysctlbyname(const char *name, void *old, size_t *oldlen, void *newp, size_t newlen);

// ---- CommonCrypto: SHA-256 ----
typedef uint32_t CC_LONG;
#define CC_SHA256_DIGEST_LENGTH 32
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } CC_SHA256_CTX;
int CC_SHA256_Init(CC_SHA256_CTX *c);
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len);
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c);
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md);

// ---- CoreFoundation: the pointer-keyed mutable dictionary hle_nvn.c uses (NULL callbacks: keys and values are raw pointers) ----
typedef struct dm_cfdict *CFMutableDictionaryRef;
typedef const struct dm_cfdict *CFDictionaryRef;
typedef long CFIndex;
typedef const void *CFAllocatorRef;
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb);
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *k);
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v);
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k);
CFIndex CFDictionaryGetCount(CFDictionaryRef d);
void CFRelease(const void *o);

// ---- Files: APFS clones and atomic swaps (the transactional save in hle_fs.c) ----
// clonefile copies the tree (with its user.* attributes) instead of sharing blocks; renamex_np(RENAME_SWAP) is renameat2(RENAME_EXCHANGE)
#define RENAME_SWAP 0x2
int clonefile(const char *src, const char *dst, uint32_t flags);
int renamex_np(const char *from, const char *to, unsigned flags);
#define F_GETPATH 50
int dm_fcntl_getpath(int fd, char *out);   // fcntl(fd, F_GETPATH, out) through /proc/self/fd
#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

// ---- malloc zones (only the Unreal allocator takeover uses them; the zone is ignored) ----
typedef struct dm_malloc_zone malloc_zone_t;
static inline malloc_zone_t *malloc_create_zone(size_t start, unsigned flags) { (void)start; (void)flags; return (malloc_zone_t *)1; }
static inline void malloc_set_zone_name(malloc_zone_t *z, const char *n) { (void)z; (void)n; }
static inline void *malloc_zone_malloc(malloc_zone_t *z, size_t n) { (void)z; return malloc(n); }
static inline void *malloc_zone_memalign(malloc_zone_t *z, size_t a, size_t n) { (void)z; return memalign(a, n); }
static inline void *malloc_zone_realloc(malloc_zone_t *z, void *p, size_t n) { (void)z; return realloc(p, n); }
static inline void malloc_zone_free(malloc_zone_t *z, void *p) { (void)z; free(p); }
static inline size_t malloc_good_size(size_t n) { return (n + 15) & ~(size_t)15; }
