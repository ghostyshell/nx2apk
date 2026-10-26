// Platform differences shared by the host and the graphics layer: Apple (Mach-O, Darwin signal frames) and Android (ELF, Linux signal frames).
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---- Registration tables (hle.h SWHLE, hle_nvn.c, the forced takeovers): every entry is a static object in a named section, read back whole at run time ----
#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#define DM_SEC(name) "__DATA," name
#define DM_SECDATA(name, szp) ((const void *)getsectiondata(&_mh_execute_header, "__DATA", #name, (szp)))
#else
// ELF: the linker defines __start_<name> / __stop_<name> around a section whose name is a C identifier. Weak, so a section no file contributes to reads as empty
#define DM_SEC(name) name
#define DM_SECDATA(name, szp) ({ extern const char __start_##name[] __attribute__((weak)), __stop_##name[] __attribute__((weak)); \
                                 *(szp) = (unsigned long)(__stop_##name - __start_##name); (const void *)(*(szp) ? __start_##name : NULL); })
#endif

// ---- Signal frames: the interrupted thread's registers ----
#if defined(__APPLE__)
#define DM_UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define DM_UC_LR(uc) ((uc)->uc_mcontext->__ss.__lr)
#define DM_UC_FP(uc) ((uc)->uc_mcontext->__ss.__fp)
#define DM_UC_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define DM_UC_X(uc, i) ((uc)->uc_mcontext->__ss.__x[i])
#define DM_UC_ESR(uc) ((uint64_t)(uc)->uc_mcontext->__es.__esr)
#define DM_UC_V(uc, i) ((uc)->uc_mcontext->__ns.__v[i])
#define DM_UC_FPSR(uc) ((uc)->uc_mcontext->__ns.__fpsr)
#define DM_UC_FPCR(uc) ((uc)->uc_mcontext->__ns.__fpcr)
#define dm_uc_has_fp(uc) 1
#else
#include <asm/sigcontext.h>
#include <ucontext.h>
#define DM_UC_PC(uc) ((uc)->uc_mcontext.pc)
#define DM_UC_LR(uc) ((uc)->uc_mcontext.regs[30])
#define DM_UC_FP(uc) ((uc)->uc_mcontext.regs[29])
#define DM_UC_SP(uc) ((uc)->uc_mcontext.sp)
#define DM_UC_X(uc, i) ((uc)->uc_mcontext.regs[i])
// ESR and the FP/SIMD registers are records in the frame's __reserved area, each starting with {magic, size}
static inline struct _aarch64_ctx *dm_uc_record(ucontext_t *uc, uint32_t magic) {
    uint8_t *p = (uint8_t *)uc->uc_mcontext.__reserved, *end = p + sizeof uc->uc_mcontext.__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        struct _aarch64_ctx *h = (struct _aarch64_ctx *)p;
        if (!h->magic || !h->size) break;
        if (h->magic == magic) return h;
        p += h->size;
    }
    return NULL;
}
static inline uint64_t dm_uc_esr(ucontext_t *uc) { struct esr_context *e = (struct esr_context *)dm_uc_record(uc, ESR_MAGIC); return e ? e->esr : 0; }
static inline struct fpsimd_context *dm_uc_fp(ucontext_t *uc) { return (struct fpsimd_context *)dm_uc_record(uc, FPSIMD_MAGIC); }
#define DM_UC_ESR(uc) dm_uc_esr(uc)
#define DM_UC_V(uc, i) (dm_uc_fp(uc)->vregs[i])
#define DM_UC_FPSR(uc) (dm_uc_fp(uc)->fpsr)
#define DM_UC_FPCR(uc) (dm_uc_fp(uc)->fpcr)
#define dm_uc_has_fp(uc) (dm_uc_fp(uc) != NULL)
#endif

// ---- The guest's thread slots (patch_nso.py emit_tsd): x18, tpidr_el0, tpidrro_el0 and the gate, one block per thread ----
// Apple: pthread keys, read by the patch code at tpidrro_el0 + 8 * key. Android: a block of 8 words whose address sits in bionic's TLS_SLOT_APP (tpidr_el0[2]),
// with the four keys at fixed indices 1-4 (0 stays unused, so a key page entry of 0 still means "not set up")
#if defined(__APPLE__)
#include <pthread.h>
typedef pthread_key_t dm_key_t;
#define dm_tsd_set(k, v) pthread_setspecific((k), (void *)(v))
#define dm_tsd_get(k) pthread_getspecific(k)
#else
typedef unsigned long dm_key_t;
static inline uint64_t *dm_tsd_block(void) {
    uint64_t **tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if (!tp[2]) { extern uint64_t *dm_tsd_block_new(void); tp[2] = dm_tsd_block_new(); }
    return tp[2];
}
#define dm_tsd_set(k, v) ((void)(dm_tsd_block()[(k)] = (uint64_t)(uintptr_t)(v)), 0)
#define dm_tsd_get(k) ((void *)(uintptr_t)dm_tsd_block()[(k)])
#endif

// ---- A descriptor's path ----
#if defined(__APPLE__)
#define DM_FCNTL_GETPATH(fd, p) fcntl((fd), F_GETPATH, (p))
#else
#define DM_FCNTL_GETPATH(fd, p) dm_fcntl_getpath((fd), (p))
#endif

// ---- Symbol names in inline assembly ----
#if defined(__APPLE__)
#define DM_ASM_SYM(n) "_" n
#define DM_ASM_HIDDEN(n) ".private_extern _" n
#define DM_ASM_PAGE(n) "_" n "@PAGE"
#define DM_ASM_PAGEOFF(n) "_" n "@PAGEOFF"
#else
#define DM_ASM_SYM(n) n
#define DM_ASM_HIDDEN(n) ".hidden " n
#define DM_ASM_PAGE(n) n
#define DM_ASM_PAGEOFF(n) ":lo12:" n
#endif
