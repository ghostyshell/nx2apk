// Null graphics backend: the replay interface hle_nvn.c drives (nvn_mtl.h and the backend's other mtl_* entry points), with no GPU behind it. For CPU bring-up on Android
// before the Vulkan replay exists. Every submission completes at once, and the replay's CPU-visible side effects are applied when it is committed: event writes,
// counter reports, buffer copies / fills and uniform updates (GPU address = CPU address, as in hle_nvn.c). Draws, clears and texture work are skipped; a texel read gives zeros.
#include "../host/hle.h"
#include "../gfx/nvn/nvn_protos.h"
#include "../gfx/nvn/nvn_tex.h"
#include "../gfx/nvn/nvn_obj.h"
#include "../gfx/nvn/nvn_mtl.h"
#include <stdatomic.h>

uint64_t guest_ns(void);   // hle_os.c: the guest clock (the source of svc GetSystemTick)

static uint64_t g_fence;   // value of the last committed submission; every one is complete when committed
static uint64_t g_seq;     // presents queued
static double g_present_t[64];

// ---------- Side effects of a recorded command list ----------
static uint64_t gpu_ticks(void) { return guest_ns() * 12 / 625; }   // 19.2 MHz, the counter timebase (as nvn_mtl.m gpu_ticks)

// DM_CENSUS=1: what the game asks of the GPU (op counts, formats, primitives), logged every 600 presents, to size the Vulkan replay
static int g_census = -1;
static int census_on(void) { if (g_census < 0) g_census = getenv("DM_CENSUS") != NULL; return g_census; }
static uint64_t g_ops[OP_COUNT_], g_seen[64]; static int g_nseen;
static void seen(char kind, uint64_t v) {
    uint64_t k = (uint64_t)kind << 56 | (v & 0xffffffffffffffull);
    for (int i = 0; i < g_nseen; i++) if (g_seen[i] == k) return;
    if (g_nseen < 64) g_seen[g_nseen++] = k;
    LOG("[census] new %c %#llx", kind, (unsigned long long)(v & 0xffffffffffffffull));
}
static void census(uint32_t op, const void *a, uint32_t size) {
    g_ops[op < OP_COUNT_ ? op : 0]++;
    switch (op) {
        case OP_SET_RT: { const p_set_rt *r = a;
            for (int i = 0; i < r->n; i++) if (r->color[i]) seen('C', (uint64_t)r->color[i]->d.format | (uint64_t)r->color[i]->d.target << 8 | (uint64_t)r->color[i]->d.width << 16 | (uint64_t)r->color[i]->d.height << 32 | (uint64_t)i << 48);
            if (r->depth) seen('D', (uint64_t)r->depth->d.format | (uint64_t)r->depth->d.width << 16 | (uint64_t)r->depth->d.height << 32);
            break; }
        case OP_BIND_VATTR: { const p_bind_vattr *v = a; for (int i = 0; i < v->n; i++) { va_st s; memcpy(&s, &v->st[i], 4); if (s.fmt) seen('A', s.fmt | (uint64_t)(v->first + i) << 8); } break; }
        case OP_DRAW: { p_draw t; const p_draw *d = draw_rec(a, size, &t); seen('P', d->prim | (uint64_t)(d->ind != 0) << 8 | (uint64_t)(d->instances > 1) << 9); break; }
        case OP_DRAW_IDX: { p_draw_idx t; const p_draw_idx *d = draw_idx_rec(a, size, &t); seen('I', d->prim | (uint64_t)d->itype << 8 | (uint64_t)(d->ind != 0) << 16 | (uint64_t)(d->instances > 1) << 17); break; }
        case OP_BIND_STATE: { const p_bind_state *b = a; if (b->kind == 3) { ms_st m; memcpy(&m, b->bytes, sizeof m); if (m.samples > 1) seen('M', m.samples); } break; }
        default: break;
    }
}
static void census_report(void) {
    char b[1024]; int n = 0;
    for (int i = 0; i < OP_COUNT_; i++) if (g_ops[i]) n += snprintf(b + n, sizeof b - n, " %d:%llu", i, (unsigned long long)(g_ops[i] / 600));
    LOG("[census] ops per frame (op:count):%s", b);
    memset(g_ops, 0, sizeof g_ops);
}

static void walk(const uint8_t *p, int depth) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        if (census_on()) census(h.op, a, h.size);
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (depth < 8) walk((const uint8_t *)t, depth + 1); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4);
                if (m == 1) __atomic_fetch_add(val, v, __ATOMIC_SEQ_CST); else __atomic_store_n(val, v, __ATOMIC_SEQ_CST);
                break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8);
                uint64_t ticks = gpu_ticks(), rec[2] = {t == 0 || t == 0x10 ? ticks : 0, ticks};
                if (ad) memcpy((void *)ad, rec, sizeof rec);
                break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1; break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); if (s && d) memmove((void *)d, (const void *)s, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); if (d) memcpy((void *)d, a + 16, n); break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; uint32_t *d = (uint32_t *)cb->addr;
                for (uint64_t i = 0; d && i < cb->size / 4; i++) d[i] = cb->value;
                break; }
            default: break;
        }
        p += h.size;
    }
}

// ---------- Submission ----------
static const uint64_t *g_rest; static int g_nrest;
void mtl_init(void) {}
void mtl_sub_begin(void) {}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { g_rest = h; g_nrest = n; }
void mtl_exec(const uint8_t *handle) { if (handle) walk(handle, 0); }
uint64_t mtl_sub_commit(void) { return ++g_fence; }
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
void mtl_finish(void) {}
void mtl_drain(void) {}
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { memmove((void *)dst, (const void *)src, n); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { if (mode == 1) __atomic_fetch_add(val, v, __ATOMIC_SEQ_CST); else __atomic_store_n(val, v, __ATOMIC_SEQ_CST); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { uint64_t t = gpu_ticks(), rec[2] = {type == 0 || type == 0x10 ? t : 0, t}; memcpy((void *)addr, rec, sizeof rec); }

// ---------- Memory: pools are the game's own memory, so an address is its own buffer ----------
void mtl_pool_add(const void *mem, uint64_t size) { (void)mem; (void)size; }
void mtl_pool_del(const void *mem) { (void)mem; }
void mtl_alias_drop_pool(const void *pool) { (void)pool; }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) { (void)len; *buf = NULL; *off = addr; return 1; }

// ---------- Objects: nothing to create ----------
void mtl_set_depth_zero(int on) { LOG("[census] depth mode near=%s", on ? "0" : "-w"); }
void mtl_set_origin_lower_left(int on) { LOG("[census] window origin %s", on ? "lower left" : "upper left"); }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_tex_cpu_written(const tex_t *t) { (void)t; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    (void)id; (void)v;
    if (census_on() && t) seen('T', (uint64_t)t->d.format | (uint64_t)t->d.target << 8 | (uint64_t)t->d.levels << 12 | (uint64_t)(t->d.samples > 1) << 20 | (uint64_t)(v && v->set_format) << 21 | (uint64_t)(v && v->set_swizzle) << 22);
}
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0;
    size_t rows = (size_t)(r->height > 0 ? r->height : 1), layers = (size_t)(r->depth > 0 ? r->depth : 1);
    for (size_t z = 0; z < layers; z++) memset(dst + z * img, 0, row * rows);
}
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }

// ---------- Presentation: no window; hle_nvn.c paces frames itself when there is no display link ----------
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)t; (void)period_s; (void)crop;
    g_seq++; g_present_t[g_seq % 64] = now_ns() / 1e9;
    if (census_on() && g_seq % 600 == 0) census_report();
    return g_seq;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { return g_present_t[n % 64]; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_has_window(void) { return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_fps_target(void) { return 60; }
int mtl_display_hz(void) { return 60; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }

// ---------- Statistics and diagnostics (all zero) ----------
void mtl_vbuf_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d) { *a = *b = *c = *d = 0; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *n) { *a = *b = 0; *n = 0; }
void mtl_args_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_bw(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_create_ns(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_create_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_pkg_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_use_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void dm_gfx_shot_at(long k, const char *path) { (void)k; (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_passdump_next(long n) { (void)n; }
