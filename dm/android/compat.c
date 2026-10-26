// Implementations behind shim/darwin_compat.h (Android).
#include "shim/darwin_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>

// ---- The guest's thread slot block (host/plat.h dm_tsd_block): 8 words per thread, reached through tpidr_el0[2] ----
static pthread_key_t g_tsd_free_key;
static pthread_once_t g_tsd_once = PTHREAD_ONCE_INIT;
static void tsd_free(void *p) {
    uint64_t **tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if (tp[2] == p) tp[2] = NULL;
    free(p);
}
static void tsd_key_init(void) { pthread_key_create(&g_tsd_free_key, tsd_free); }
uint64_t *dm_tsd_block_new(void) {
    pthread_once(&g_tsd_once, tsd_key_init);
    uint64_t *b = calloc(8, sizeof *b);
    if (!b) abort();
    pthread_setspecific(g_tsd_free_key, b);   // freed when the thread exits
    return b;
}

// ---- Mach memory ----
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out) {
    (void)task;
    struct iovec l = {(void *)data, size}, r = {(void *)addr, size};
    ssize_t n = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (out) *out = n > 0 ? (vm_size_t)n : 0;
    return n == (ssize_t)size ? KERN_SUCCESS : KERN_INVALID_ADDRESS;
}
kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags) {
    (void)task;
    void *p = mmap((flags & VM_FLAGS_ANYWHERE) ? NULL : (void *)*addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | ((flags & VM_FLAGS_ANYWHERE) ? 0 : MAP_FIXED), -1, 0);
    if (p == MAP_FAILED) return KERN_FAILURE;
    *addr = (vm_address_t)p;
    return KERN_SUCCESS;
}
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size) { (void)task; return munmap((void *)addr, size) ? KERN_FAILURE : KERN_SUCCESS; }
kern_return_t vm_remap(mach_port_t task, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t src_task, vm_address_t from, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh) {
    (void)task; (void)to; (void)size; (void)mask; (void)flags; (void)src_task; (void)from; (void)copy; (void)inh;
    if (cur) *cur = 0;
    if (max) *max = 0;
    return KERN_NOT_SUPPORTED;
}

// ---- Threads ----
int dm_setname1(const char *name) { char b[16]; strncpy(b, name, 15); b[15] = 0; return (pthread_setname_np)(pthread_self(), b); }
static int stack_range(pthread_t t, uint8_t **lo, size_t *sz) {
    pthread_attr_t a;
    if (pthread_getattr_np(t, &a)) return -1;
    void *p; size_t n; int r = pthread_attr_getstack(&a, &p, &n);
    pthread_attr_destroy(&a);
    *lo = p; *sz = n;
    return r;
}
void *pthread_get_stackaddr_np(pthread_t t) { uint8_t *lo; size_t n; return stack_range(t, &lo, &n) ? NULL : lo + n; }
size_t pthread_get_stacksize_np(pthread_t t) { uint8_t *lo; size_t n; return stack_range(t, &lo, &n) ? 0 : n; }
int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec abs = {now.tv_sec + rel->tv_sec, now.tv_nsec + rel->tv_nsec};
    if (abs.tv_nsec >= 1000000000) { abs.tv_sec++; abs.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait_monotonic_np(c, m, &abs);
}
// QoS to nice: interactive threads (the game thread, audio) above normal, utility / background below. Raising priority above 0 can be refused; that is fine
static __thread qos_class_t t_qos = QOS_CLASS_UNSPECIFIED;
int pthread_set_qos_class_self_np(qos_class_t q, int rel) {
    (void)rel;
    int nice = q == QOS_CLASS_USER_INTERACTIVE ? -10 : q == QOS_CLASS_USER_INITIATED ? -4 : q == QOS_CLASS_UTILITY ? 4 : q == QOS_CLASS_BACKGROUND ? 10 : 0;
    t_qos = q;
    setpriority(PRIO_PROCESS, (id_t)gettid(), nice);
    return 0;
}
int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel) {
    if (q) *q = pthread_equal(t, pthread_self()) ? t_qos : QOS_CLASS_UNSPECIFIED;
    if (rel) *rel = 0;
    return 0;
}

// ---- Dispatch semaphores ----
struct dm_dsema { sem_t s; };
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) {
    if (when == DISPATCH_TIME_FOREVER) return DISPATCH_TIME_FOREVER;
    uint64_t base = when == DISPATCH_TIME_NOW ? dm_mono_ns(CLOCK_MONOTONIC) : when;
    return delta < 0 && (uint64_t)-delta > base ? 1 : base + (uint64_t)delta;
}
dispatch_semaphore_t dispatch_semaphore_create(long value) {
    struct dm_dsema *d = malloc(sizeof *d);
    if (!d || sem_init(&d->s, 0, (unsigned)value)) { free(d); return NULL; }
    return d;
}
long dispatch_semaphore_signal(dispatch_semaphore_t d) { return sem_post(&d->s) ? -1 : 0; }
long dispatch_semaphore_wait(dispatch_semaphore_t d, dispatch_time_t t) {
    if (t == DISPATCH_TIME_FOREVER) { while (sem_wait(&d->s) && errno == EINTR) {} return 0; }
    struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
    for (;;) {
        if (!sem_timedwait_monotonic_np(&d->s, &ts)) return 0;
        if (errno != EINTR) return 1;
    }
}
void dispatch_release(void *o) { struct dm_dsema *d = o; if (d) { sem_destroy(&d->s); free(d); } }

// ---- sysctl: the few queries the host makes ----
int sysctlbyname(const char *name, void *old, size_t *oldlen, void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (!strcmp(name, "hw.memsize") && old && oldlen && *oldlen >= 8) {
        struct sysinfo si; sysinfo(&si);
        *(uint64_t *)old = (uint64_t)si.totalram * si.mem_unit; *oldlen = 8;
        return 0;
    }
    if ((!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.logicalcpu") || !strcmp(name, "hw.activecpu")) && old && oldlen && *oldlen >= 4) {
        *(int *)old = (int)sysconf(_SC_NPROCESSORS_ONLN); *oldlen = 4;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

// ---- SHA-256 (FIPS 180-4) ----
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, k;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0;
    return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len) {
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        uint32_t t = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, t); c->n += t; p += t; len -= t;
        if (c->n == 64) { sha256_block(c->h, c->buf); c->n = 0; }
    }
    return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    CC_SHA256_Update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) CC_SHA256_Update(c, &pad, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(c, lb, 8);
    for (int i = 0; i < 8; i++) { md[4 * i] = (uint8_t)(c->h[i] >> 24); md[4 * i + 1] = (uint8_t)(c->h[i] >> 16); md[4 * i + 2] = (uint8_t)(c->h[i] >> 8); md[4 * i + 3] = (uint8_t)c->h[i]; }
    return 1;
}
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) { CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, data, len); CC_SHA256_Final(md, &c); return md; }

// ---- CFMutableDictionary with raw pointer keys: open addressing, grows at 1/2 load ----
struct dm_cfdict { const void **k, **v; size_t cap, n; };
static size_t ph(const void *k, size_t cap) { uint64_t x = (uint64_t)(uintptr_t)k; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return (size_t)x & (cap - 1); }
#define TOMB ((const void *)1)
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb) {
    (void)a; (void)cap; (void)kcb; (void)vcb;
    struct dm_cfdict *d = calloc(1, sizeof *d);
    d->cap = 64; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v);
    return d;
}
static long find(const struct dm_cfdict *d, const void *k) {
    for (size_t i = ph(k, d->cap), s = 0; s < d->cap; i = (i + 1) & (d->cap - 1), s++) {
        if (!d->k[i]) return -1;
        if (d->k[i] == k) return (long)i;
    }
    return -1;
}
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *k) { long i = find(d, k); return i < 0 ? NULL : d->v[i]; }
static void put(struct dm_cfdict *d, const void *k, const void *v) {
    size_t i = ph(k, d->cap);
    while (d->k[i] && d->k[i] != TOMB) i = (i + 1) & (d->cap - 1);
    d->k[i] = k; d->v[i] = v; d->n++;
}
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) {
    long i = find(d, k);
    if (i >= 0) { d->v[i] = v; return; }
    if (2 * (d->n + 1) > d->cap) {
        struct dm_cfdict o = *d;
        d->cap *= 2; d->n = 0; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v);
        for (size_t j = 0; j < o.cap; j++) if (o.k[j] && o.k[j] != TOMB) put(d, o.k[j], o.v[j]);
        free(o.k); free(o.v);
    }
    put(d, k, v);
}
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k) { long i = find(d, k); if (i >= 0) { d->k[i] = TOMB; d->v[i] = NULL; d->n--; } }
CFIndex CFDictionaryGetCount(CFDictionaryRef d) { return (CFIndex)d->n; }
void CFRelease(const void *o) { (void)o; }

// ---- Files ----
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
static int copy_xattrs(const char *src, const char *dst) {
    char names[4096]; ssize_t n = llistxattr(src, names, sizeof names);
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(names + i) + 1) {
        char v[4096]; ssize_t k = lgetxattr(src, names + i, v, sizeof v);
        // A filesystem that keeps no user attributes (shell-owned /data/local/tmp refuses them) loses nothing the copy needs: they only record case-collision names
        if (k >= 0 && lsetxattr(dst, names + i, v, (size_t)k, 0) && errno != EACCES && errno != EPERM && errno != ENOTSUP) return -1;
    }
    return 0;
}
static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777);
    int r = in < 0 || out < 0 ? -1 : 0;
    char buf[1 << 16];
    for (ssize_t k; !r && (k = read(in, buf, sizeof buf)) != 0;) {
        if (k < 0) { if (errno == EINTR) continue; r = -1; break; }
        for (ssize_t w = 0; w < k;) { ssize_t m = write(out, buf + w, (size_t)(k - w)); if (m < 0) { if (errno == EINTR) continue; r = -1; break; } w += m; }
    }
    int e = errno;
    if (in >= 0) close(in);
    if (out >= 0 && close(out)) r = -1;
    errno = e;
    return r ? -1 : copy_xattrs(src, dst);
}
int clonefile(const char *src, const char *dst, uint32_t flags) {
    (void)flags;
    struct stat st;
    if (lstat(src, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst, st.st_mode);
    if (mkdir(dst, st.st_mode & 07777)) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    int r = 0;
    for (struct dirent *e; !r && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[4096], b[4096];
        snprintf(a, sizeof a, "%s/%s", src, e->d_name); snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        r = clonefile(a, b, 0);
    }
    closedir(d);
    return r ? -1 : copy_xattrs(src, dst);
}
int renamex_np(const char *from, const char *to, unsigned flags) {
    if (flags & RENAME_SWAP) return (int)syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, 2 /* RENAME_EXCHANGE */);
    return rename(from, to);
}
int dm_fcntl_getpath(int fd, char *out) {
    char p[64]; snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, out, MAXPATHLEN - 1);
    if (n < 0) return -1;
    out[n] = 0;
    return 0;
}

// ---- AudioUnit over AAudio (shim/AudioToolbox/AudioToolbox.h) ----
#include "shim/AudioToolbox/AudioToolbox.h"
struct dm_aucomp { int unused; };
struct dm_au { AAudioStream *s; AURenderCallbackStruct cb; AudioStreamBasicDescription fmt; int have_fmt; };
static struct dm_aucomp g_output_comp;
AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d->componentType == kAudioUnitType_Output ? &g_output_comp : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioUnit *out) { (void)c; *out = calloc(1, sizeof **out); return *out ? noErr : -1; }
OSStatus AudioUnitSetProperty(AudioUnit u, UInt32 id, UInt32 scope, UInt32 elem, const void *data, UInt32 size) {
    (void)scope; (void)elem;
    if (id == kAudioUnitProperty_StreamFormat && size == sizeof u->fmt) { memcpy(&u->fmt, data, sizeof u->fmt); u->have_fmt = 1; return noErr; }
    if (id == kAudioUnitProperty_SetRenderCallback && size == sizeof u->cb) { memcpy(&u->cb, data, sizeof u->cb); return noErr; }
    return -10879;   // kAudioUnitErr_InvalidProperty
}
static aaudio_data_callback_result_t au_data(AAudioStream *s, void *user, void *data, int32_t frames) {
    (void)s;
    struct dm_au *u = user;
    AudioBufferList bl = {1, {{u->fmt.mChannelsPerFrame, (UInt32)frames * u->fmt.mBytesPerFrame, data}}};
    AudioUnitRenderActionFlags f = 0; AudioTimeStamp ts = {0, 0};
    u->cb.inputProc(u->cb.inputProcRefCon, &f, &ts, 0, (UInt32)frames, &bl);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
OSStatus AudioUnitInitialize(AudioUnit u) {
    if (!u->have_fmt || !u->cb.inputProc || u->fmt.mBitsPerChannel != 16) return -10868;   // kAudioUnitErr_FormatNotSupported
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return -1;
    AAudioStreamBuilder_setSampleRate(b, (int32_t)u->fmt.mSampleRate);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)u->fmt.mChannelsPerFrame);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(b, au_data, u);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &u->s);
    AAudioStreamBuilder_delete(b);
    return r == AAUDIO_OK ? noErr : (OSStatus)r;
}
OSStatus AudioOutputUnitStart(AudioUnit u) { return u->s && AAudioStream_requestStart(u->s) == AAUDIO_OK ? noErr : -1; }
OSStatus AudioOutputUnitStop(AudioUnit u) {
    if (!u->s || AAudioStream_requestStop(u->s) != AAUDIO_OK) return -1;
    // Apple's AudioOutputUnitStop returns once the callback has stopped; AAudio's requestStop is asynchronous and keeps calling back until STOPPING ends.
    // The host relies on the synchronous form: FlushAudioOutBuffers rewrites the queues right after StopAudioOut.
    // Wait in 5 ms slices, re-reading the state each time (waitForStateChange reports TIMEOUT for a slice that simply did not change); give up after 100 ms,
    // which the caller holds under g_aos_lock and the watchdog needs every 50 ms
    for (int i = 0; i < 20; i++) {
        aaudio_stream_state_t st = AAudioStream_getState(u->s);
        if (st == AAUDIO_STREAM_STATE_STOPPED || st == AAUDIO_STREAM_STATE_CLOSED) break;
        aaudio_stream_state_t next;
        AAudioStream_waitForStateChange(u->s, st, &next, 5000000);   // timeout just means another slice is needed
    }
    return noErr;
}
OSStatus AudioUnitUninitialize(AudioUnit u) { if (u->s) { AAudioStream_close(u->s); u->s = NULL; } return noErr; }
OSStatus AudioComponentInstanceDispose(AudioUnit u) { if (u) { AudioUnitUninitialize(u); free(u); } return noErr; }
uint64_t dm_mach_absolute_time(void) { return mach_absolute_time(); }   // called from the converted assembly (elfasm.py)
