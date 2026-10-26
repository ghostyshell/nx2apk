// Vulkan replay of the NVN commands hle_nvn.c records, for Android (Mali-G76 on the S10e): the replay interface of nvn_mtl.h / nvn_obj.h that nvn_mtl.m implements on Metal,
// cut to what Devil May Cry uses (DM_CENSUS in nvn_null.c): indexed triangles, RGBA8 colour and D24S8 depth targets, RGBA8 / BC7 / depth textures, uniform buffers, combined
// texture-samplers, no compute, storage buffers or images.
// Model:
//   · memory: the Mali driver has no VK_EXT_external_memory_host, so the game's pools cannot back Vulkan buffers. What a submission's draws read (index ranges, the vertex ranges
//     those indices reach, uniform blocks) is copied at replay time into a ring buffer in coherent device memory; a texture is deswizzled (and BC-decoded: Mali has no BC) into the
//     ring and copied into its image by a pre-pass command buffer submitted ahead of the submission's own. The GPU never reads game memory, so the copies are a snapshot of the
//     submission as the game left it, which NVN's rules make the same thing
//   · textures: one VkImage per texture serial, created at first use. A render target, copy destination or clear target becomes GPU-owned and is never uploaded again; any other
//     texture is uploaded from its storage and again when its pages change (nvn_dirty write protection, as nvn_mtl.m pkg_upload). Every image stays in GENERAL layout, and a full
//     memory barrier separates render passes and copies (Mali does not use layouts beyond PRESENT / UNDEFINED)
//   · shaders: the offline translations from dm/android/tools/vk_sh_build.py (<DM_VK_SHADERS>/<key>.spv, vk_shaders.tsv); vertex shaders use set 0, pixel shaders set 1, one
//     descriptor set per stage per draw whose bindings changed, from a pool owned by the submission's slot
//   · completion: a timeline semaphore carries the fence values; event writes and counter reports wait for their submission on a completion thread, as Metal's completion handlers
//   · clip space: the shaders are translated for Metal (y up, origin upper left, depth 0..1); a negative-height viewport gives Vulkan the same mapping
// Environment: DM_VK_SHADERS=<dir> (default: vksh beside the executable); DM_VK_SHOT=<dir>[:every] writes presented frames as PPM (no window); DM_VK_LOG=1 logs object creation
#include "../host/hle.h"
#include "../gfx/nvn/nvn_protos.h"
#include "../gfx/nvn/nvn_tex.h"
#include "../gfx/nvn/nvn_obj.h"
#include "../gfx/nvn/nvn_mtl.h"
#include "../gfx/nvn/nvn_hmap.h"
#include "../gfx/nvn/nvn_dirty.h"
#include "../gfx/nvn/nvn_primitive.h"
#include "../gfx/tex/nvn_deswizzle.h"
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <stdatomic.h>
#include <libgen.h>
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

uint64_t guest_ns(void);   // hle_os.c: the guest clock (the source of svc GetSystemTick)
extern __attribute__((weak, visibility("default"))) ANativeWindow *dm_android_window;   // front_android.c (the NativeActivity build): the surface to present to; NULL in the adb shell build

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
#define RED1(...) do { static int once_; if (!once_) { once_ = 1; LOG("[vk] [RED] " __VA_ARGS__); } } while (0)
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) die("[vk] %s failed: %d (%s:%d)", #x, r_, __FILE__, __LINE__); } while (0)
static int g_log;
static uint64_t g_trace_frame; static int g_trace;   // 1: passes and GPU times, 2 (DM_VK_TRACE_DRAWS): every draw too   // DM_VK_TRACE=<present number>: log that frame's passes, draws and the present
#define OLOG(...) do { if (g_log) VLOG(__VA_ARGS__); } while (0)

// ---------- Device ----------
static VkInstance g_inst; static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;
static VkPhysicalDeviceMemoryProperties g_mp; static VkPhysicalDeviceProperties g_props;
static VkSemaphore g_tl;   // timeline: value n = submission n complete
static PFN_vkWaitSemaphoresKHR p_wait_sems; static PFN_vkGetSemaphoreCounterValueKHR p_sem_value;
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;   // the queue (submits come from the submit thread, presents from the present thread)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;    // texture / sampler / descriptor tables and everything the replay touches
// Held from mtl_sub_begin to the submission's commit: the game's submit thread and the present thread (mtl_present, shot) each open submissions of their own, and the
// replay keeps one open submission's state (command buffers, slot, draw state); g_mu alone is dropped between mtl_exec calls
static pthread_mutex_t g_open_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_fence, g_done;
static _Atomic uint64_t g_replay_ns, g_gpu_ns;   // per-frame statistics: CPU time replaying submissions, GPU time (commit to completion, overlapping submissions counted once)
static uint64_t g_commit_t[64];
static _Atomic uint64_t g_wait_ns[3], g_wait_n[3];   // CPU blocked on the GPU: 0 the game's syncs (mtl_wait), 1 submission slot reuse, 2 ring reuse   // last submitted value, last value the completion thread saw complete
static int g_depth_zero = 1;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_mp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_mp.memoryTypes[i].propertyFlags & want) == want) return i;
    die("[vk] no memory type %#x in %#x", want, bits);
}
static VkDeviceMemory mem_alloc(VkMemoryRequirements r, VkMemoryPropertyFlags want) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size, .memoryTypeIndex = mem_type(r.memoryTypeBits, want)};
    VkDeviceMemory m; VKC(vkAllocateMemory(g_dev, &ai, NULL, &m)); return m;
}
static VkBuffer buf_new(VkDeviceSize n, VkBufferUsageFlags use, VkMemoryPropertyFlags want, VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = use};
    VkBuffer b; VKC(vkCreateBuffer(g_dev, &bi, NULL, &b));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(g_dev, b, &r);
    *mem = mem_alloc(r, want); VKC(vkBindBufferMemory(g_dev, b, *mem, 0));
    if (map) VKC(vkMapMemory(g_dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
    return b;
}

// ---------- Deferred destruction: objects the GPU may still use go when the submission that last could have used them completes ----------
typedef struct { uint64_t fence; int kind; uint64_t h, m; } dead_t;   // kind: 0 image + memory, 1 view, 2 buffer + memory, 3 framebuffer
static dead_t *g_dead; static size_t g_ndead, g_cdead;
static void dead_add(int kind, uint64_t h, uint64_t m) {
    if (g_ndead == g_cdead && !(g_dead = realloc(g_dead, (g_cdead = g_cdead ? 2 * g_cdead : 256) * sizeof *g_dead))) die("[vk] dead list");
    g_dead[g_ndead++] = (dead_t){atomic_load(&g_fence) + 1, kind, h, m};
}
static void dead_reap(void) {
    uint64_t done = atomic_load(&g_done); size_t k = 0;
    for (size_t i = 0; i < g_ndead; i++) {
        dead_t *d = &g_dead[i];
        if (d->fence > done) { g_dead[k++] = *d; continue; }
        switch (d->kind) {
            case 0: vkDestroyImage(g_dev, (VkImage)d->h, NULL); if (d->m) vkFreeMemory(g_dev, (VkDeviceMemory)d->m, NULL); break;
            case 1: vkDestroyImageView(g_dev, (VkImageView)d->h, NULL); break;
            case 2: vkDestroyBuffer(g_dev, (VkBuffer)d->h, NULL); vkFreeMemory(g_dev, (VkDeviceMemory)d->m, NULL); break;
            case 3: vkDestroyFramebuffer(g_dev, (VkFramebuffer)d->h, NULL); break;
        }
    }
    g_ndead = k;
}

// ---------- Ring: per-submission copies of vertex / index / uniform data and texture staging ----------
enum { RING_SIZE = 96 << 20, RING_SEGS = 96 };   // fence per 1 MiB segment
static VkBuffer g_ring; static VkDeviceMemory g_ring_mem; static uint8_t *g_ring_map;
static uint64_t g_ring_head, g_seg_fence[RING_SEGS];
static uint64_t g_ring_bytes;   // copied this frame (statistics)
static int vk_wait(uint64_t v, uint64_t timeout_ns);
static uint64_t g_sub_ring;   // ring bytes of the open submission: one submission must not lap the ring onto its own copies
static VkDeviceSize ring_alloc(VkDeviceSize n, VkDeviceSize align) {
    if (n > RING_SIZE / 4) die("[vk] ring allocation of %llu bytes", (unsigned long long)n);
    const uint64_t SEG = RING_SIZE / RING_SEGS;
    uint64_t o = (g_ring_head + align - 1) & ~(align - 1);
    int wrap = o + n > RING_SIZE; if (wrap) o = 0;
    uint64_t cur = atomic_load(&g_fence) + 1, s0 = o / SEG;
    if (!wrap && g_ring_head && (g_ring_head - 1) / SEG == s0) s0++;   // the segment the head is in holds earlier data of the open lap: nothing to wait for there
    for (uint64_t s = s0; s <= (o + n - 1) / SEG; s++) {   // segments entered for the first time this lap: their last use must be complete
        if (g_seg_fence[s] && g_seg_fence[s] != cur && g_seg_fence[s] > atomic_load(&g_done)) { uint64_t t = now_ns(); vk_wait(g_seg_fence[s], UINT64_MAX); g_wait_ns[2] += now_ns() - t; g_wait_n[2]++; }
    }
    for (uint64_t s = o / SEG; s <= (o + n - 1) / SEG; s++) g_seg_fence[s] = cur;
    g_ring_head = o + n; g_ring_bytes += n; g_sub_ring += n + align;
    if (g_sub_ring > RING_SIZE - 2 * SEG) die("[vk] one submission copied %llu MB of draw data, more than the ring holds", (unsigned long long)(g_sub_ring >> 20));
    return o;
}
static VkDeviceSize ring_put(const void *src, size_t n, VkDeviceSize align) { VkDeviceSize o = ring_alloc(n, align); memcpy(g_ring_map + o, src, n); return o; }

// ---------- Submission slots: command buffers and descriptor pools, reused when the slot's last submission completes ----------
enum { NSLOT = 6 };
typedef struct { VkCommandPool pool; VkCommandBuffer pre, cmd; VkDescriptorPool dp[16]; int ndp, curdp; uint64_t fence; } slot_t;
static slot_t g_slot[NSLOT]; static int g_cur_slot;
static VkCommandBuffer g_pre, g_cmd; static int g_pre_used, g_rec; static int g_pre_war;   // the open submission's command buffers; g_rec: recording

// post-completion effects of a submission (event writes, counter reports), applied by the completion thread
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, type; } post_t;
typedef struct post_list { struct post_list *next; uint64_t fence; int n, cap; post_t *op; } post_list;
static post_list *g_post_head, *g_post_tail, *g_post_cur;
static pthread_mutex_t g_post_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_post_cv = PTHREAD_COND_INITIALIZER;
static uint64_t gpu_ticks(void) { return guest_ns() * 12 / 625; }   // 19.2 MHz, the counter timebase (as nvn_mtl.m gpu_ticks)
static void post_add(post_t p) {
    post_list *l = g_post_cur;
    if (!l) { l = g_post_cur = calloc(1, sizeof *l); if (!l) die("[vk] post list"); }
    if (l->n == l->cap && !(l->op = realloc(l->op, (l->cap = l->cap ? 2 * l->cap : 8) * sizeof *l->op))) die("[vk] post list");
    l->op[l->n++] = p;
}
static void post_apply(const post_t *p) {
    if (p->kind == 1) { if (p->mode == 1) __atomic_fetch_add(p->val, p->v, __ATOMIC_SEQ_CST); else __atomic_store_n(p->val, p->v, __ATOMIC_SEQ_CST); }
    else { uint64_t t = gpu_ticks(), rec[2] = {p->type == 0 || p->type == 0x10 ? t : 0, t}; memcpy((void *)p->addr, rec, sizeof rec); }
}
static void *completion_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "dm-vk-done");
    for (uint64_t next = 1;; next++) {
        while (atomic_load(&g_fence) < next) { pthread_mutex_lock(&g_post_mu); if (atomic_load(&g_fence) < next) pthread_cond_wait(&g_post_cv, &g_post_mu); pthread_mutex_unlock(&g_post_mu); }
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_tl, .pValues = &next};
        VkResult r; while ((r = p_wait_sems(g_dev, &wi, 1000000000ull)) == VK_TIMEOUT) RED1("submission %llu still running after 1 s", (unsigned long long)next);
        if (r != VK_SUCCESS) die("[vk] vkWaitSemaphores: %d (device lost?)", r);
        pthread_mutex_lock(&g_post_mu);
        while (g_post_head && g_post_head->fence <= next) {
            post_list *l = g_post_head; g_post_head = l->next; if (!g_post_head) g_post_tail = NULL;
            pthread_mutex_unlock(&g_post_mu);
            for (int i = 0; i < l->n; i++) post_apply(&l->op[i]);
            free(l->op); free(l);
            pthread_mutex_lock(&g_post_mu);
        }
        { static uint64_t last_end; uint64_t t = now_ns(), st = g_commit_t[next % 64]; if (st < last_end) st = last_end; if (t > st) atomic_fetch_add(&g_gpu_ns, t - st); last_end = t; }
        atomic_store(&g_done, next);
        pthread_cond_broadcast(&g_post_cv);
        pthread_mutex_unlock(&g_post_mu);
    }
    return NULL;
}
static int vk_wait(uint64_t v, uint64_t timeout_ns) {
    if (atomic_load(&g_done) >= v) return 1;
    uint64_t end = now_ns() + (timeout_ns > (1ull << 62) ? (1ull << 62) : timeout_ns);
    pthread_mutex_lock(&g_post_mu);
    while (atomic_load(&g_done) < v) {
        uint64_t t = now_ns(); if (t >= end) break;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); uint64_t w = end - t > 100000000ull ? 100000000ull : end - t;
        ts.tv_nsec += (long)w; ts.tv_sec += ts.tv_nsec / 1000000000; ts.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&g_post_cv, &g_post_mu, &ts);
    }
    int ok = atomic_load(&g_done) >= v; pthread_mutex_unlock(&g_post_mu);
    return ok;
}

// ---------- Shaders ----------
enum { RK_SUP, RK_C1, RK_UBO, RK_TEX };
typedef struct { uint8_t kind, binding; uint16_t reg; uint32_t need; } sres;   // reg: uniform slot / texture slot; need: the bytes of a uniform block the shader reads (0 = all)
typedef struct shader {
    char key[17]; int stage;   // 0 vertex, 1 pixel
    uint8_t *c1; uint32_t c1_len, c1_off;   // c1_off: offset in g_c1buf
    sres res[16]; int nres; uint32_t inputs, int_inputs;   // vertex input locations (bits); those declared uvec4 (an integer attribute read as raw bits)
    VkShaderModule mod[2];     // vertex: [0] GL depth rewrite, [1] near = 0 (<key>_z); pixel: [0]
    VkDescriptorSetLayout dsl;
} shader;
static nvn_hmap g_sh;   // 16-hex key → shader
static char g_sh_dir[512];
static VkBuffer g_c1buf, g_supbuf, g_zerobuf; static VkDeviceMemory g_c1mem, g_supmem, g_zeromem;
static VkDescriptorSetLayout g_empty_dsl;
static void shaders_load(void) {
    const char *e = getenv("DM_VK_SHADERS");
    if (e) snprintf(g_sh_dir, sizeof g_sh_dir, "%s", e);
    else { char exe[512]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); exe[n > 0 ? n : 0] = 0; snprintf(g_sh_dir, sizeof g_sh_dir, "%s/vksh", dirname(exe)); }
    char p[600]; snprintf(p, sizeof p, "%s/vk_shaders.tsv", g_sh_dir);
    FILE *f = fopen(p, "r"); if (!f) die("[vk] cannot read the shader table %s (dm/android/tools/vk_sh_build.py; DM_VK_SHADERS names its folder)", p);
    nvn_hm_init(&g_sh, 16);
    static char ln[8192]; int n = 0; uint32_t c1_total = 0;
    while (fgets(ln, sizeof ln, f)) {
        if (ln[0] == '#' || ln[0] == '\n') continue;
        char *col[5] = {0}; int nc = 0; for (char *s = ln, *t; nc < 5 && (t = strsep(&s, "\t\n")); ) col[nc++] = t;
        if (nc < 4) continue;
        shader *s = calloc(1, sizeof *s); if (!s) die("[vk] shader");
        snprintf(s->key, sizeof s->key, "%s", col[0]); s->stage = !strcmp(col[1], "ps");
        if (strcmp(col[2], "-")) { size_t l = strlen(col[2]) / 2; s->c1 = malloc(l); s->c1_len = (uint32_t)l; for (size_t i = 0; i < l; i++) sscanf(col[2] + 2 * i, "%2hhx", &s->c1[i]); s->c1_off = c1_total; c1_total += (uint32_t)((l + 255) & ~255u); }
        for (char *r = col[3], *t; (t = strsep(&r, " ")); ) {
            if (!*t) continue;
            char *eq = strchr(t, '='); if (!eq || s->nres == 16) continue;
            sres x = {0, (uint8_t)atoi(eq + 1), 0};
            if (!strncmp(t, "sup", 3)) x.kind = RK_SUP;
            else if (!strncmp(t, "c1", 2)) x.kind = RK_C1;
            else if (t[0] == 'u') { x.kind = RK_UBO; x.reg = (uint16_t)atoi(t + 1); char *c = strchr(eq, ':'); x.need = c ? (uint32_t)atoi(c + 1) : 0; }
            else if (t[0] == 't' || t[0] == 'T') { x.kind = RK_TEX; x.reg = (uint16_t)atoi(t + 1); if (t[0] == 'T') RED1("shader %s uses a separate sampler (%s): not implemented", s->key, t); }
            else { RED1("shader %s resource %s not implemented", s->key, t); continue; }
            s->res[s->nres++] = x;
        }
        if (nc > 4 && strcmp(col[4], "-")) for (char *r = col[4], *t; (t = strsep(&r, ",")); ) if (*t) { s->inputs |= 1u << atoi(t); if (strchr(t, 'u')) s->int_inputs |= 1u << atoi(t); }
        VkDescriptorSetLayoutBinding b[16]; VkShaderStageFlags sf = s->stage ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        for (int i = 0; i < s->nres; i++) b[i] = (VkDescriptorSetLayoutBinding){s->res[i].binding, s->res[i].kind == RK_TEX ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sf, NULL};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)s->nres, .pBindings = b};
        VKC(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &s->dsl));
        nvn_hm_put(&g_sh, s->key, s); n++;
    }
    fclose(f);
    // c1: every shader's constants at its offset, each read through a 64 KiB range (the translated shaders declare 4096 vec4s), so the buffer reaches 64 KiB past the last
    void *map; g_c1buf = buf_new(c1_total + 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g_c1mem, &map);
    memset(map, 0, c1_total + 65536);
    for (uint32_t i = 0; i < g_sh.cap; i++) { shader *s = g_sh.vals[i]; if (s && s->c1) memcpy((uint8_t *)map + s->c1_off, s->c1, s->c1_len); }
    // The translator's support buffer (Ryujinx SupportBuffer): no alpha test, no BGRA targets, render scale 1
    g_supbuf = buf_new(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g_supmem, &map);
    memset(map, 0, 65536); for (int i = 0; i < 73; i++) { float one = 1.0f; memcpy((uint8_t *)map + 192 + 16 * i, &one, 4); }
    g_zerobuf = buf_new(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g_zeromem, &map);
    memset(map, 0, 65536);
    VLOG("shaders: %d from %s (c1 %u bytes)", n, g_sh_dir, c1_total);
}
static VkShaderModule sh_module(shader *s) {
    int v = s->stage == 0 && g_depth_zero;
    if (s->mod[v]) return s->mod[v];
    char p[600]; snprintf(p, sizeof p, "%s/%s%s.spv", g_sh_dir, s->key, v ? "_z" : "");
    FILE *f = fopen(p, "rb"); if (!f) die("[vk] missing %s", p);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)n); if (fread(w, 1, (size_t)n, f) != (size_t)n) die("[vk] read %s", p); fclose(f);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = w};
    VKC(vkCreateShaderModule(g_dev, &ci, NULL, &s->mod[v])); free(w);
    return s->mod[v];
}
// A program's stages → shaders (by serial; the keys are hle_nvn.c prog_key's)
typedef struct { shader *vs, *fs; } prog_res;
static nvn_hmap g_progs;
static prog_res *prog_get(const prog_t *p) {
    prog_res *r = nvn_hm_get(&g_progs, &p->serial);
    if (r) return r;
    r = calloc(1, sizeof *r);
    for (unsigned st = 0; st < p->nstage; st++) {
        uint8_t k[8]; char hex[17]; if (!prog_key(p, st, k)) { VLOG("[RED] program %llu stage %u: no key", (unsigned long long)p->serial, st); continue; }
        for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", k[i]);
        shader *s = nvn_hm_get(&g_sh, hex);
        if (!s) { VLOG("[RED] program %llu stage %u: shader %s is not in the table", (unsigned long long)p->serial, st, hex); continue; }
        if (s->stage) r->fs = s; else r->vs = s;
    }
    nvn_hm_put(&g_progs, &p->serial, r);
    OLOG("program %llu: %u stages, vs %s fs %s", (unsigned long long)p->serial, p->nstage, r->vs ? r->vs->key : "-", r->fs ? r->fs->key : "-");
    return r;
}

// ---------- Formats ----------
// decode: 0 none, 1 BC1, 2 BC2, 3 BC3, 4 BC4, 5 BC5, 6 BC6H (half, signed in bit 3), 7 BC7; the image then has the decoded format
typedef struct { VkFormat vk; uint8_t decode, noalpha, depth; } fmt_t;
static fmt_t fmt_of(int f) {
    static const VkFormat plain[0x41] = {
        [0x01] = VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
        VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM,
        VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM,
        VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        VK_FORMAT_S8_UINT, VK_FORMAT_D16_UNORM, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, 0, 0, 0, 0,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
    };
    fmt_t r = {0, 0, 0, 0};
    if (f > 0 && f < 0x41) { r.vk = plain[f]; r.noalpha = f == 0x37; r.depth = f >= 0x31 && f <= 0x36; return r; }
    if (f >= 0x41 && f <= 0x48) { int srgb = f >= 0x45, k = (f - 0x41) & 3; r.vk = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM; r.decode = k <= 1 ? 1 : (uint8_t)k; r.noalpha = k == 0; return r; }
    switch (f) {
        case 0x49: r.vk = VK_FORMAT_R8_UNORM; r.decode = 4; return r;
        case 0x4A: r.vk = VK_FORMAT_R8_UNORM; r.decode = 4; RED1("BC4 signed decoded as unsigned"); return r;
        case 0x4B: r.vk = VK_FORMAT_R8G8_UNORM; r.decode = 5; return r;
        case 0x4C: r.vk = VK_FORMAT_R8G8_UNORM; r.decode = 5; RED1("BC5 signed decoded as unsigned"); return r;
        case 0x4D: r.vk = VK_FORMAT_R8G8B8A8_UNORM; r.decode = 7; return r;
        case 0x4E: r.vk = VK_FORMAT_R8G8B8A8_SRGB; r.decode = 7; return r;
        case 0x4F: r.vk = VK_FORMAT_R16G16B16A16_SFLOAT; r.decode = 6 | 8; return r;
        case 0x50: r.vk = VK_FORMAT_R16G16B16A16_SFLOAT; r.decode = 6; return r;
        case 0x99: r.noalpha = 1; /* fall through */
        case 0x9A: r.vk = VK_FORMAT_B8G8R8A8_UNORM; return r;
        case 0x9B: r.noalpha = 1; /* fall through */
        case 0x9C: r.vk = VK_FORMAT_B8G8R8A8_SRGB; return r;
    }
    if (f >= 0x79 && f <= 0x94) {   // ASTC: 14 LDR then 14 sRGB, same block order as Vulkan's
        static const VkFormat a[14] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
            VK_FORMAT_ASTC_8x5_UNORM_BLOCK, VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_ASTC_10x6_UNORM_BLOCK,
            VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
        int k = (f - 0x79) % 14; r.vk = (VkFormat)(a[k] + (f >= 0x87 ? 1 : 0));   // each _SRGB_BLOCK follows its _UNORM_BLOCK
        return r;
    }
    return r;
}
static VkFormat vfmt_of(int f) {   // vertex attribute formats
    switch (f) {   // *_UI2F / *_I2F: integer data read as float without normalising: the USCALED / SSCALED formats
        case 0x51: return VK_FORMAT_R8_USCALED; case 0x52: return VK_FORMAT_R8_SSCALED; case 0x53: return VK_FORMAT_R16_USCALED; case 0x54: return VK_FORMAT_R16_SSCALED;
        case 0x57: return VK_FORMAT_R8G8_USCALED; case 0x58: return VK_FORMAT_R8G8_SSCALED; case 0x59: return VK_FORMAT_R16G16_USCALED; case 0x5A: return VK_FORMAT_R16G16_SSCALED;
        case 0x5D: return VK_FORMAT_R8G8B8_USCALED; case 0x5E: return VK_FORMAT_R8G8B8_SSCALED; case 0x5F: return VK_FORMAT_R16G16B16_USCALED; case 0x60: return VK_FORMAT_R16G16B16_SSCALED;
        case 0x63: return VK_FORMAT_R8G8B8A8_USCALED; case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED; case 0x65: return VK_FORMAT_R16G16B16A16_USCALED; case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED;
        case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    }
    fmt_t t = fmt_of(f); return t.decode ? 0 : t.vk;
}
static int is_depth(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT; }
static VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return is_depth(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
static VkFormatFeatureFlags fmt_features(VkFormat f) {
    static VkFormatFeatureFlags cache[256]; static uint8_t have[256];
    unsigned i = (unsigned)f < 256 ? (unsigned)f : 0;
    if (i && have[i]) return cache[i];
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(g_pd, f, &p);
    if (i) { cache[i] = p.optimalTilingFeatures; have[i] = 1; }
    return p.optimalTilingFeatures;
}

// ---------- Textures ----------
typedef struct vview { struct vview *next; uint64_t key; VkImageView v; } vview;
typedef struct vtex {
    uint64_t serial; int refs;
    nvn_texdesc d; nvn_texlayout L; const uint8_t *mem; fmt_t f;
    VkImage img; VkDeviceMemory dm; VkImageAspectFlags aspect;
    uint32_t w, h, z, layers, levels; VkImageViewType vt;
    int gpu, filled; uint64_t stamp; const void *span;   // gpu: rendered / copied / cleared into (not uploaded again until the CPU writes its storage)
    int mutable_fmt;   // created with MUTABLE_FORMAT (views may use other formats)
    int rewrites; uint64_t up_sub, stream_from;   // rewrites: uploads in a row found written again (streaming from STREAM_AFTER on); up_sub: the submission of its last upload
    _Atomic int cpu_written;   // WriteTexels or a host write (the movie decoder) since the last upload: the storage is the content again
    vview *views;
} vtex;
static nvn_hmap g_tex;   // serial → vtex
static uint64_t g_uploads, g_upload_bytes;
// Descriptor pools: the snapshot of each registered texture (with its view) and sampler, by host ID
typedef struct { tex_t t; view_t v; uint8_t has_v, live; } treg_t;
static treg_t *g_treg; static int g_ntreg;
typedef struct { samp_par p; VkSampler s; uint8_t live; } sreg_t;
static sreg_t *g_sreg; static int g_nsreg;
static nvn_hmap g_samp_cache;   // samp_par → VkSampler

static uint32_t mipd(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
static vtex *tex_find(uint64_t serial) { return serial ? nvn_hm_get(&g_tex, &serial) : NULL; }
static vtex *tex_get(const tex_t *t) {
    if (!t || !t->serial) return NULL;
    vtex *x = nvn_hm_get(&g_tex, &t->serial);
    if (x) return x;
    fmt_t f = fmt_of(t->d.format);
    if (!f.vk) { RED1("texture format %#x has no Vulkan format (first: %ux%u)", t->d.format, t->d.width, t->d.height); return NULL; }
    if (t->d.samples > 1) RED1("multisampled texture (%d samples) created single-sampled", t->d.samples);
    x = calloc(1, sizeof *x); if (!x) die("[vk] vtex");
    x->serial = t->serial; x->refs = 1; x->d = t->d; x->L = t->L; x->mem = t->mem; x->f = f; x->aspect = aspect_of(f.vk);
    x->w = t->d.width ? t->d.width : 1; x->h = t->d.height ? t->d.height : 1; x->z = 1; x->layers = 1; x->levels = t->d.levels ? t->d.levels : 1;
    VkImageType it = VK_IMAGE_TYPE_2D; VkImageCreateFlags cf = 0; x->vt = VK_IMAGE_VIEW_TYPE_2D;
    switch (t->d.target) {
        case NVN_TEXTURE_TARGET_1D: it = VK_IMAGE_TYPE_1D; x->h = 1; x->vt = VK_IMAGE_VIEW_TYPE_1D; break;
        case NVN_TEXTURE_TARGET_1D_ARRAY: it = VK_IMAGE_TYPE_1D; x->layers = x->h; x->h = 1; x->vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
        case NVN_TEXTURE_TARGET_3D: it = VK_IMAGE_TYPE_3D; x->z = t->d.depth ? t->d.depth : 1; x->vt = VK_IMAGE_VIEW_TYPE_3D; break;
        case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: x->layers = t->d.depth ? t->d.depth : 1; x->vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case NVN_TEXTURE_TARGET_CUBEMAP: x->layers = 6; x->vt = VK_IMAGE_VIEW_TYPE_CUBE; cf |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: x->layers = t->d.depth ? t->d.depth : 6; x->vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; cf |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
        case NVN_TEXTURE_TARGET_BUFFER: RED1("buffer textures not implemented"); free(x); return NULL;
        default: break;
    }
    VkFormatFeatureFlags ff = fmt_features(f.vk);
    VkImageUsageFlags use = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) use |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) use |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    // Reinterpreting views (another format on the same image) need MUTABLE_FORMAT, which keeps the Mali driver from compressing the image (AFBC): DMC never makes one, and
    //   without the flag its frames take about 20% fewer GPU cycles. DM_VK_MUTABLE=1 creates every colour image mutable, for a game that does (tex_view logs the first case)
    static int mut = -1; if (mut < 0) mut = getenv("DM_VK_MUTABLE") != NULL;
    if (mut && !f.depth && !(f.vk >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f.vk <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)) { cf |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; x->mutable_fmt = 1; }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cf, .imageType = it, .format = f.vk, .extent = {x->w, x->h, x->z}, .mipLevels = x->levels,
                            .arrayLayers = x->layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VKC(vkCreateImage(g_dev, &ci, NULL, &x->img));
    VkMemoryRequirements r; vkGetImageMemoryRequirements(g_dev, x->img, &r);
    x->dm = mem_alloc(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); VKC(vkBindImageMemory(g_dev, x->img, x->dm, 0));
    // UNDEFINED → GENERAL, cleared to zero (a texture the game reads before writing reads 0, as on the Switch's freshly mapped memory); in the pre-pass, ahead of everything
    VkImageSubresourceRange all = {x->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = x->img, .subresourceRange = all};
    vkCmdPipelineBarrier(g_pre, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    if (f.depth) { VkClearDepthStencilValue z = {0, 0}; vkCmdClearDepthStencilImage(g_pre, x->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all); }
    else if (!(f.vk >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f.vk <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)) { VkClearColorValue z = {{0}}; vkCmdClearColorImage(g_pre, x->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all); }
    g_pre_used = 1;
    nvn_hm_put(&g_tex, &x->serial, x);
    OLOG("texture %llu %ux%ux%u layers %u levels %u format %#x → %d%s", (unsigned long long)x->serial, x->w, x->h, x->z, x->layers, x->levels, t->d.format, f.vk, f.decode ? " (decoded)" : "");
    return x;
}
static void tex_free(vtex *x) {
    for (vview *v = x->views, *n; v; v = n) { n = v->next; dead_add(1, (uint64_t)v->v, 0); free(v); }
    dead_add(0, (uint64_t)x->img, (uint64_t)x->dm);
    free(x);
}
static void fb_drop_tex(const vtex *x);
void mtl_tex_init(const tex_t *t) { (void)t; }
static void tex_release_now(uint64_t serial) {
    if (!serial || !g_dev) return;
    pthread_mutex_lock(&g_mu);
    vtex *x = nvn_hm_del(&g_tex, &serial);
    if (x) { fb_drop_tex(x); tex_free(x); }
    pthread_mutex_unlock(&g_mu);
}
// WriteTexels and host writes (the movie decoder's native-texture frames): the storage holds the texture's content now, even for one the GPU cleared or drew into before
// (DMC clears its movie texture, then the decoder writes every frame into its storage). Other textures the GPU has not written are checked against their pages (tex_upload)
// unless_gpu: only if the GPU has not drawn, copied or cleared into it (nvn_ft_poll's page hits), decided where the replay has reached
static void tex_cpu_written_now(uint64_t serial, int unless_gpu) {
    if (!serial || !g_dev) return;
    pthread_mutex_lock(&g_mu); vtex *x = tex_find(serial); if (x && !(unless_gpu && x->gpu)) atomic_store(&x->cpu_written, 1); pthread_mutex_unlock(&g_mu);
    OLOG("CPU wrote texture %llu%s", (unsigned long long)serial, x ? "" : " (no host texture yet)");
}
int mtl_tex_gpu_owned(uint64_t serial) { pthread_mutex_lock(&g_mu); vtex *x = g_dev ? tex_find(serial) : NULL; int r = x && x->gpu; pthread_mutex_unlock(&g_mu); return r; }

// Upload a texture from its storage when the GPU has not written it and it was never filled or its pages changed since
static void barrier_full(VkCommandBuffer cb);
// The pre-pass's first command: its clears and copies come after earlier submissions' reads and writes of the same images (fragment and transfer stages only, so the GPU's
// vertex work of this submission can still overlap the previous one), then each upload after the clears before it
// (g_pre_war: the pre-pass has its barrier after earlier submissions)
static void pre_use(void) {
    if (g_pre_war) return;
    g_pre_war = 1;
    VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(g_pre, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &m, 0, NULL, 0, NULL);
    g_pre_used = 1;
}
static void pre_transfer_barrier(void) {
    VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(g_pre, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &m, 0, NULL, 0, NULL);
}
// Texture staging: small uploads in the ring; a large one, or any once the submission has used a quarter of the ring, in a buffer of its own freed after the submission (a level
// load decodes tens of MB of BC textures in one submission, and a 4096x2048 BC7 level alone decodes to 32 MB)
static uint8_t *stage_alloc(size_t n, VkBuffer *buf, VkDeviceSize *off) {
    if (n <= (1u << 20) && g_sub_ring + n < RING_SIZE / 4) { *buf = g_ring; *off = ring_alloc(n, 16); return g_ring_map + *off; }
    VkDeviceMemory m; void *map; *buf = buf_new(n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &m, &map);
    dead_add(2, (uint64_t)*buf, (uint64_t)m); *off = 0; return map;
}
// A texture the game rewrites on the CPU for STREAM_AFTER uploads in a row (the movie's luma / chroma planes, every frame) is no longer write-protected: it is uploaded once in each
//   submission that uses it. Protecting it again after every upload cost a page fault per 4 KiB page per frame (12% of the CPU in movies)
enum { STREAM_AFTER = 8, STREAM_RECHECK = 600 };   // STREAM_RECHECK submissions later it is tracked again, so a texture that settles stops being uploaded
static uint64_t g_sub_n;   // submissions begun
static uint64_t g_cur_job;   // the job the replay thread is running (OP_FENCE_SYNC)
// Texture patches (texpatch.c): an RGBA8 level is passed to tex_patch after it is decoded into staging, by the hash of its source bytes
void tex_patch(uint64_t hash, uint32_t lv, uint32_t w, uint32_t h, uint8_t *rgba, size_t pitch);
int tex_patch_wants(uint32_t lv, uint32_t w, uint32_t h);
static uint64_t fnv64(const uint8_t *p, size_t n, uint64_t h) { for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull; return h; }
static int rgba8(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB; }   // patches are RGBA
static void tex_upload(vtex *x) {
    int cw = atomic_exchange(&x->cpu_written, 0);
    if (cw) x->gpu = 0;
    if (x->gpu || !x->mem || x->f.depth) return;
    int streaming = x->rewrites >= STREAM_AFTER;
    int recheck = streaming && g_sub_n - x->stream_from >= STREAM_RECHECK;
    if (recheck) { x->rewrites = 0; streaming = 0; }   // back to page tracking: uploaded and protected again below (its pages were not tracked)
    else if (streaming) { if (x->up_sub == g_sub_n) return; }
    else {
        if (!cw && x->filled && x->span && !nvn_dirty_since_span(x->span, x->mem, x->L.size, x->stamp)) { x->rewrites = 0; return; }
        if (!cw && x->filled && !x->span) return;   // untracked storage: filled once (and on CPU writes)
        if (x->filled && x->span && !cw && ++x->rewrites == STREAM_AFTER) x->stream_from = g_sub_n;
    }
    if (!streaming) nvn_dirty_protect(x->mem, x->L.size);
    uint64_t st = nvn_dirty_now();   // protect, stamp, then read: a later write is seen at the next check
    int bw, bh, bpb = nvn_fmt_bpb(x->d.format, &bw, &bh);
    if (!bpb) { RED1("texture format %#x: no block size", x->d.format); return; }
    int linear = x->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET);
    if (linear && (x->levels > 1 || x->layers > 1)) RED1("linear texture with levels / layers: only level 0 uploaded");
    pre_use(); pre_transfer_barrier();   // after the image's creation clear
    for (uint32_t lv = 0; lv < x->levels; lv++) {
        uint32_t w = mipd(x->w, lv), h = mipd(x->h, lv), z = x->d.target == NVN_TEXTURE_TARGET_3D ? mipd(x->z, lv) : 1;
        uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
        size_t pitch = (size_t)bx * bpb, raw = pitch * by * z;
        for (uint32_t ly = 0; ly < x->layers; ly++) {
            if (linear && !x->f.decode) {   // rows straight into the staging memory
                VkBuffer sb; VkDeviceSize off; uint8_t *d = stage_alloc(raw, &sb, &off); size_t st_ = x->d.stride ? x->d.stride : pitch;
                for (uint32_t y = 0; y < by; y++) memcpy(d + y * pitch, x->mem + y * st_, pitch);
                VkBufferImageCopy reg = {off, bx * bw, by * bh, {x->aspect, lv, ly, 1}, {0, 0, 0}, {w, h, z}};
                vkCmdCopyBufferToImage(g_pre, sb, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &reg); g_upload_bytes += raw;
                continue;
            }
            uint8_t *lin = malloc(raw); if (!lin) die("[vk] upload");
            if (linear) { size_t st_ = x->d.stride ? x->d.stride : pitch; for (uint32_t y = 0; y < by; y++) memcpy(lin + y * pitch, x->mem + y * st_, pitch); }
            else if (nvn_deswizzle(&x->d, lv, ly, x->mem, lin, pitch)) { RED1("deswizzle failed (format %#x target %d)", x->d.format, x->d.target); free(lin); return; }
            VkDeviceSize off; size_t n; VkBuffer sb; VkBufferImageCopy reg;
            if (x->f.decode) {   // BC → the decoded format, one 4x4 block at a time
                int k = x->f.decode & 7, opx = k == 4 ? 1 : k == 5 ? 2 : k == 6 ? 8 : 4;
                size_t dpitch = (size_t)bx * 4 * opx; n = dpitch * by * 4 * z;
                uint8_t *d = stage_alloc(n, &sb, &off);
                for (uint32_t zz = 0; zz < z; zz++) for (uint32_t y = 0; y < by; y++) for (uint32_t xx = 0; xx < bx; xx++) {
                    const uint8_t *s = lin + ((size_t)zz * by + y) * pitch + (size_t)xx * bpb; uint8_t *o = d + ((size_t)zz * by * 4 + y * 4) * dpitch + (size_t)xx * 4 * opx;
                    switch (k) {
                        case 1: bcdec_bc1(s, o, (int)dpitch); break; case 2: bcdec_bc2(s, o, (int)dpitch); break; case 3: bcdec_bc3(s, o, (int)dpitch); break;
                        case 4: bcdec_bc4(s, o, (int)dpitch); break; case 5: bcdec_bc5(s, o, (int)dpitch); break; case 7: bcdec_bc7(s, o, (int)dpitch); break;
                        case 6: { uint16_t rgb[48]; bcdec_bc6h_half(s, rgb, 12, x->f.decode & 8);
                            for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { uint16_t *q = (uint16_t *)(o + r * dpitch) + c * 4; q[0] = rgb[r * 12 + c * 3]; q[1] = rgb[r * 12 + c * 3 + 1]; q[2] = rgb[r * 12 + c * 3 + 2]; q[3] = 0x3c00; } break; }
                    }
                }
                reg = (VkBufferImageCopy){off, bx * 4, by * 4, {x->aspect, lv, ly, 1}, {0, 0, 0}, {w, h, z}};
                if (opx == 4 && rgba8(x->f.vk) && ly == 0 && z == 1 && tex_patch_wants(lv, w, h)) tex_patch(fnv64(lin, raw, 0xcbf29ce484222325ull ^ lv), lv, w, h, d, dpitch);
            } else {
                n = raw; uint8_t *d = stage_alloc(raw, &sb, &off); memcpy(d, lin, raw);
                if (bpb == 4 && bw == 1 && rgba8(x->f.vk) && ly == 0 && z == 1 && tex_patch_wants(lv, w, h)) tex_patch(fnv64(lin, raw, 0xcbf29ce484222325ull ^ lv), lv, w, h, d, pitch);
                reg = (VkBufferImageCopy){off, bx * bw, by * bh, {x->aspect, lv, ly, 1}, {0, 0, 0}, {w, h, z}};
            }
            vkCmdCopyBufferToImage(g_pre, sb, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &reg);
            free(lin); g_upload_bytes += n;
        }
    }
    g_pre_used = 1; g_uploads++;
    OLOG("upload texture %llu %ux%u format %#x target %d flags %#x stride %u levels %u layout %d/%d%s", (unsigned long long)x->serial, x->w, x->h, x->d.format, x->d.target, x->d.flags, x->d.stride, x->levels, x->d.pkg_block_h, x->L.tileH, cw ? " (CPU written)" : "");
    x->filled = 1; x->stamp = st; x->span = nvn_dirty_span(x->mem, x->L.size); x->up_sub = g_sub_n;
}
static VkComponentSwizzle swz1(int s) { return (VkComponentSwizzle)(s >= 0 && s <= 5 ? s + 1 : 0); }   // NVN 0 zero 1 one 2 R 3 G 4 B 5 A → Vulkan ZERO .. A
static VkImageView tex_view(vtex *x, const tex_t *t, const view_t *v, int has_v, int rt) {
    int fmt = has_v && v->set_format ? v->format : x->d.format;
    unsigned bl = has_v && v->set_levels ? v->base_level : 0, nl = has_v && v->set_levels && v->nlevels ? v->nlevels : x->levels - bl;
    unsigned l0 = has_v && v->set_layers ? v->min_layer : 0, nly = has_v && v->set_layers && v->nlayers ? v->nlayers : x->layers - l0;
    if (rt) nl = 1;
    uint8_t sw[4] = {2, 3, 4, 5};
    if (t && t->set_swz) for (int i = 0; i < 4; i++) sw[i] = (uint8_t)(t->swz >> (3 * i) & 7);
    if (has_v && v->set_swizzle) { uint8_t c[4]; for (int i = 0; i < 4; i++) { uint8_t s = v->swz[i]; c[i] = s >= 2 && s <= 5 ? sw[s - 2] : s; } memcpy(sw, c, 4); }
    fmt_t f = fmt_of(fmt); if (!f.vk || f.depth != x->f.depth) f = x->f;
    if (f.vk != x->f.vk && !x->mutable_fmt) { RED1("view format %#x on a texture of format %#x: drawn in the texture's own format (DM_VK_MUTABLE=1 allows reinterpreting views)", fmt, x->d.format); f.vk = x->f.vk; }
    if (f.noalpha) sw[3] = 1;
    if (rt) memcpy(sw, (uint8_t[4]){2, 3, 4, 5}, 4);
    int stencil = (t && t->stencil) || (has_v && v->set_dsmode && v->dsmode == 1);
    uint64_t key = ((uint64_t)f.vk ^ (uint64_t)(has_v && v->set_target ? v->target + 1 : 0) << 12) | (uint64_t)bl << 16 | (uint64_t)nl << 20 | (uint64_t)l0 << 26 | (uint64_t)(nly & 0x3ff) << 38 | (uint64_t)(sw[0] | sw[1] << 3 | sw[2] << 6 | sw[3] << 9) << 48 | (uint64_t)stencil << 60 | (uint64_t)rt << 61;
    for (vview *w = x->views; w; w = w->next) if (w->key == key) return w->v;
    VkImageViewType vt = x->vt;
    if (rt || (vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY && nly == 1 && has_v && v->set_target && v->target == NVN_TEXTURE_TARGET_2D)) vt = x->vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_2D : x->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkImageAspectFlags asp = x->aspect;
    if (!rt && (asp & VK_IMAGE_ASPECT_DEPTH_BIT)) asp = stencil && (asp & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;   // a sampled view names one aspect
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = x->img, .viewType = vt, .format = f.vk,
                                .components = {swz1(sw[0]), swz1(sw[1]), swz1(sw[2]), swz1(sw[3])}, .subresourceRange = {asp, bl, nl, l0, rt ? 1 : nly}};
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = rt ? (x->f.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) : VK_IMAGE_USAGE_SAMPLED_BIT};
    if (f.vk != x->f.vk) ci.pNext = &uci;   // a reinterpreting view may not support the image's other uses
    vview *w = calloc(1, sizeof *w); w->key = key;
    VKC(vkCreateImageView(g_dev, &ci, NULL, &w->v));
    w->next = x->views; x->views = w;
    return w->v;
}

// ---------- Samplers ----------
static VkSampler samp_get(const samp_par *p) {
    VkSampler s = nvn_hm_get(&g_samp_cache, p);
    if (s) return s;
    static const VkSamplerAddressMode wrap[8] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    int mip = p->minf >= 2;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = p->magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (p->minf == 1 || p->minf == 3 || p->minf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = p->minf >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap[p->ws & 7], .addressModeV = wrap[p->wt & 7], .addressModeW = wrap[p->wr & 7], .mipLodBias = p->lod_bias,
        .anisotropyEnable = p->aniso > 1, .maxAnisotropy = p->aniso > 1 ? (p->aniso > 16 ? 16 : p->aniso) : 1,
        .compareEnable = p->cmp_mode == 1, .compareOp = (VkCompareOp)(p->cmp_func >= 1 && p->cmp_func <= 8 ? p->cmp_func - 1 : 7),
        .minLod = mip ? p->lod_min : 0, .maxLod = mip ? p->lod_max : 0.25f,   // no mipmaps: level 0, with the min / mag choice kept (GL's convention)
        .borderColor = p->border[3] == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : p->border[0] == 0 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    VKC(vkCreateSampler(g_dev, &ci, NULL, &s));
    nvn_hm_put(&g_samp_cache, p, s);
    return s;
}
void mtl_set_tex_pool_n(int n) { (void)n; }
static void reg_tex_now(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id > (1 << 20)) return;
    pthread_mutex_lock(&g_mu);
    if (id >= g_ntreg) { int n = id + 1024; g_treg = realloc(g_treg, (size_t)n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (size_t)(n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    treg_t *r = &g_treg[id]; r->live = t != NULL;
    if (t) { r->t = *t; r->has_v = v != NULL; if (v) r->v = *v; }
    pthread_mutex_unlock(&g_mu);
}
static void reg_samp_now(int id, const samp_par *p) {
    if (id < 0 || id > (1 << 12) || !p) return;
    pthread_mutex_lock(&g_mu);
    if (id >= g_nsreg) { int n = id + 256; g_sreg = realloc(g_sreg, (size_t)n * sizeof *g_sreg); memset(g_sreg + g_nsreg, 0, (size_t)(n - g_nsreg) * sizeof *g_sreg); g_nsreg = n; }
    g_sreg[id].p = *p; g_sreg[id].live = 1; g_sreg[id].s = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_mu);
}
static void prog_release_now(uint64_t serial) {
    if (!serial || !g_dev) return;
    pthread_mutex_lock(&g_mu); free(nvn_hm_del(&g_progs, &serial)); pthread_mutex_unlock(&g_mu);
}

// ---------- Render passes and framebuffers ----------
// load[i] / store[i]: attachment i (8 = depth, 9 = stencil) 0 load / store, 1 clear / don't care. Framebuffers and pipelines use the all-load / all-store pass of the same formats (compatible)
typedef struct { VkFormat c[8]; VkFormat d; uint32_t n; uint8_t load[10], store[10]; } rp_key;
static nvn_hmap g_rps;
static VkRenderPass rp_get(const rp_key *k) {
    VkRenderPass rp = nvn_hm_get(&g_rps, k);
    if (rp) return rp;
    VkAttachmentDescription a[9]; VkAttachmentReference cr[8], dr; uint32_t na = 0;
#define LD(i) (k->load[i] ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD)
#define ST(i) (k->store[i] ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE)
    for (uint32_t i = 0; i < k->n; i++) {
        if (!k->c[i]) { cr[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL}; continue; }
        a[na] = (VkAttachmentDescription){0, k->c[i], VK_SAMPLE_COUNT_1_BIT, LD(i), ST(i), VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                          VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
        cr[i] = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL};
    }
    if (k->d) {
        a[na] = (VkAttachmentDescription){0, k->d, VK_SAMPLE_COUNT_1_BIT, LD(8), ST(8), LD(9), ST(9), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
        dr = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL};
    }
#undef LD
#undef ST
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, k->n, cr, NULL, k->d ? &dr : NULL, 0, NULL};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = a, .subpassCount = 1, .pSubpasses = &sp};
    VKC(vkCreateRenderPass(g_dev, &ci, NULL, &rp));
    nvn_hm_put(&g_rps, k, rp);
    return rp;
}
typedef struct { VkRenderPass rp; VkImageView v[9]; uint32_t w, h; uint64_t ser[9]; VkFramebuffer fb; } fb_ent;
enum { NFB = 128 };
static fb_ent g_fb[NFB]; static int g_nfb, g_fb_next;
static VkFramebuffer fb_get(VkRenderPass rp, const VkImageView *v, const uint64_t *ser, uint32_t n, uint32_t w, uint32_t h) {
    for (int i = 0; i < g_nfb; i++) if (g_fb[i].rp == rp && g_fb[i].w == w && g_fb[i].h == h && !memcmp(g_fb[i].v, v, n * sizeof *v) && (n == 9 || !g_fb[i].v[n])) return g_fb[i].fb;
    fb_ent *e;
    if (g_nfb < NFB) e = &g_fb[g_nfb++]; else { e = &g_fb[g_fb_next]; g_fb_next = (g_fb_next + 1) % NFB; dead_add(3, (uint64_t)e->fb, 0); }
    memset(e, 0, sizeof *e); e->rp = rp; e->w = w; e->h = h; memcpy(e->v, v, n * sizeof *v); memcpy(e->ser, ser, n * sizeof *ser);
    VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = n, .pAttachments = v, .width = w, .height = h, .layers = 1};
    VKC(vkCreateFramebuffer(g_dev, &ci, NULL, &e->fb));
    return e->fb;
}
static void fb_drop_tex(const vtex *x) {   // framebuffers over a texture's views go with it
    for (int i = 0; i < g_nfb; i++) for (int k = 0; k < 9; k++) if (g_fb[i].ser[k] == x->serial && g_fb[i].fb) {
        dead_add(3, (uint64_t)g_fb[i].fb, 0); g_fb[i] = g_fb[--g_nfb]; i--; break; }
    g_fb_next = 0;
}

// ---------- Draw state ----------
typedef struct {
    // render targets
    p_set_rt rt; tex_t rt_tex[9]; int rt_valid;
    // pipeline inputs
    prog_res cur, *pr;   // the shaders bound per stage (a program binds the stages in its mask)
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; vs_st stream[16]; NVNbufferRange vbuf[16];
    int restart; uint32_t restart_index; int discard;
    // dynamic
    float vp[16][4]; float dnear, dfar; p_scissor sc; float bias[3]; float blendc[4];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    // resources
    struct { uint64_t addr, size, copied; VkDeviceSize ring; } ubo[2][16];   // copied: bytes in the ring for this binding (0: none yet)
    uint64_t tex[2][32];   // combined handles by slot (tid | sid << 20)
    int dirty_set[2];
} draw_state;
static draw_state S;
static VkRenderPass g_rp; static int g_in_rp; static uint32_t g_rp_w, g_rp_h; static VkPipeline g_bound_pipe; static int g_dyn_dirty;
static uint64_t g_passes, g_draws, g_frame_passes, g_frame_draws, g_seq;
#define TR(...) do { if (g_trace) VLOG("[trace] " __VA_ARGS__); } while (0)
static VkDescriptorSet g_set[2]; static VkPipelineLayout g_bound_layout;

static void state_reset(void) {
    memset(&S, 0, sizeof S);
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
    S.dnear = 0; S.dfar = 1; S.sc = (p_scissor){0, 0, 1 << 16, 1 << 16};
    for (int f = 0; f < 2; f++) { S.st_vmask[f] = S.st_wmask[f] = 0xff; }
    S.dirty_set[0] = S.dirty_set[1] = 1; g_dyn_dirty = 1; g_bound_pipe = VK_NULL_HANDLE; g_bound_layout = VK_NULL_HANDLE;
}

static void barrier_full(VkCommandBuffer cb) {
    VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m, 0, NULL, 0, NULL);
}
// Between render passes and copies: what attachments and transfers wrote, before fragment shading, attachments and transfers read it. No shader before the fragment stage reads
// a texture DMC renders (its vertex shaders sample nothing), so the next pass's vertex work need not wait
static void barrier_pass(VkCommandBuffer cb) {
    VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    VkPipelineStageFlags src = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkPipelineStageFlags dst = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &m, 0, NULL, 0, NULL);
}

// ---------- Pending clears: a whole-attachment clear outside a render pass waits for the next pass, where it becomes the attachment's load op ----------
typedef struct { uint64_t serial; VkClearValue v; VkImageAspectFlags asp; unsigned level; } pend_t;
static pend_t g_pend[16]; static int g_npend;
static void pend_flush(void);
static void pend_add(vtex *x, unsigned level, VkImageAspectFlags asp, VkClearValue v) {
    for (int i = 0; i < g_npend; i++) if (g_pend[i].serial == x->serial && g_pend[i].level == level) {
        if (asp & VK_IMAGE_ASPECT_DEPTH_BIT) g_pend[i].v.depthStencil.depth = v.depthStencil.depth;
        if (asp & VK_IMAGE_ASPECT_STENCIL_BIT) g_pend[i].v.depthStencil.stencil = v.depthStencil.stencil;
        if (asp & VK_IMAGE_ASPECT_COLOR_BIT) g_pend[i].v = v;
        g_pend[i].asp |= asp; return; }
    if (g_npend == 16) pend_flush();
    g_pend[g_npend++] = (pend_t){x->serial, v, asp, level};
}
static int pend_take(uint64_t serial, unsigned level, pend_t *out) {
    for (int i = 0; i < g_npend; i++) if (g_pend[i].serial == serial && g_pend[i].level == level) { *out = g_pend[i]; g_pend[i] = g_pend[--g_npend]; return 1; }
    return 0;
}
static void pend_flush(void) {   // the pending clears as image clears (outside a render pass)
    if (!g_npend) return;
    barrier_pass(g_cmd);
    for (int i = 0; i < g_npend; i++) {
        vtex *x = tex_find(g_pend[i].serial); if (!x) continue;
        VkImageSubresourceRange r = {g_pend[i].asp, g_pend[i].level, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        if (x->f.depth) vkCmdClearDepthStencilImage(g_cmd, x->img, VK_IMAGE_LAYOUT_GENERAL, &g_pend[i].v.depthStencil, 1, &r);
        else vkCmdClearColorImage(g_cmd, x->img, VK_IMAGE_LAYOUT_GENERAL, &g_pend[i].v.color, 1, &r);
    }
    g_npend = 0;
}

// ---------- Store lookahead: is an attachment's content read after this pass, or fully cleared first? ----------
// Scans the rest of the submission from the record after the current one (g_wstack, innermost call first, then the handles still to come). 1 = read or unknown (store), 0 = cleared before any read
static const uint8_t *g_wstack[9]; static int g_wdepth;   // the record after the current one at each call depth of the replay
static const uint64_t *g_rest; static int g_nrest;
typedef struct { uint64_t serial; int depth; int attached, in_cur; int asp_cleared; int bound; } look_t;
static int look_rec(look_t *L, const uint8_t *p, int depth) {   // -1 = go on, 0 / 1 = decided
    for (int guard = 0; guard < 1000000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        switch (h.op) {
            case OP_END: return -1;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (depth >= 8) return 1; int r = look_rec(L, (const uint8_t *)t, depth + 1); if (r >= 0) return r; break; }
            case OP_SET_RT: { const p_set_rt *r = a; L->in_cur = 0; L->attached = 0;
                if (L->depth) L->attached = r->depth && r->depth->serial == L->serial;
                else for (int i = 0; i < r->n && i < 8; i++) if (r->color[i] && r->color[i]->serial == L->serial) L->attached = i + 1;
                break; }
            case OP_CLEAR_DS: if (!L->in_cur && L->attached && L->depth) { const p_clear_ds *c = a; if (c->depth_mask) L->asp_cleared |= 1; if (c->stencil_mask == 0xff || (c->stencil_mask & 0xff) == 0xff) L->asp_cleared |= 2; if (L->asp_cleared == 3) return 0; } break;
            case OP_CLEAR_COLOR: if (!L->in_cur && L->attached && !L->depth) { const p_clear_color *c = a; if (c->index == L->attached - 1 && (c->mask & 0xf) == 0xf) return 0; } break;
            case OP_CLEAR_TEX: { const p_clear_tex *c = a; if (c->tex && c->tex->serial == L->serial && !L->in_cur) { if (c->r.xoffset == 0 && c->r.yoffset == 0 && (uint32_t)c->r.width >= c->tex->d.width && (uint32_t)c->r.height >= c->tex->d.height && (c->mask & 0xf) == 0xf) return 0; return 1; } break; }
            case OP_DRAW: case OP_DRAW_IDX: if (!L->in_cur && (L->attached || L->bound)) return 1; break;
            case OP_COPY_T2T: { const p_copy_t2t *c = a; if (c->src && c->src->serial == L->serial) return 1; if (c->dst && c->dst->serial == L->serial) return 1; break; }
            case OP_BIND_TEX1: case OP_BIND_TEX: { p_bind_handles t; const p_bind_handles *b = bind_rec(h.op, a, &t);
                for (int i = 0; i < b->n; i++) { int tid = (int)(b->h[i] & 0xfffff); if (tid && tid < g_ntreg && g_treg[tid].t.serial == L->serial) return 1; }
                break; }
            case OP_COPY_B2T: case OP_COPY_T2B: case OP_DISPATCH: case OP_BIND_IMG: return 1;
        }
        p += h.size;
    }
    return 1;
}
static int look_read(uint64_t serial, int depth, int stencil_only_ok) {
    (void)stencil_only_ok;
    look_t L = {serial, depth, 1, 1, 0, 0};
    // bound now (a binding made before this pass stays in effect): any draw of a later pass may sample it
    for (int st = 0; st < 2; st++) for (int i = 0; i < 32; i++) { int tid = (int)(S.tex[st][i] & 0xfffff); if (tid && tid < g_ntreg && g_treg[tid].t.serial == serial) L.bound = 1; }
    for (int d = g_wdepth; d >= 0; d--) if (g_wstack[d]) { int r = look_rec(&L, g_wstack[d], 0); if (r >= 0) return r; }
    for (int i = 0; i < g_nrest; i++) { int r = look_rec(&L, (const uint8_t *)g_rest[i], 0); if (r >= 0) return r; }
    return 1;   // end of the submission: the next one may read it
}

static void ts_mark(const char *name);
static uint64_t g_pass_draws;
static void rp_end(void) { if (g_in_rp) { vkCmdEndRenderPass(g_cmd); g_in_rp = 0; if (g_trace) { char nm[48]; snprintf(nm, sizeof nm, "pass (%llu draws)", (unsigned long long)g_pass_draws); ts_mark(nm); } g_pass_draws = 0; } }
// Open the render pass of the current targets (at the first draw after SetRenderTargets): pending clears of its attachments become load ops, other pending clears are done first;
// an attachment cleared before anything reads it again is not stored
static VkRenderPass g_rp_compat;
// DM_VK_TRACE frame: GPU timestamps around each pass, read back when the frame's submissions complete
static VkQueryPool g_tsq; static uint32_t g_nts; static char g_ts_name[64][48]; static uint64_t g_ts_fence;
static void ts_mark(const char *name) {
    if (!g_trace || g_nts >= 64) return;
    if (!g_tsq) { VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 64}; VKC(vkCreateQueryPool(g_dev, &ci, NULL, &g_tsq)); }
    if (!g_nts) vkCmdResetQueryPool(g_pre, g_tsq, 0, 64), g_pre_used = 1;
    snprintf(g_ts_name[g_nts], sizeof g_ts_name[0], "%s", name);
    vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsq, g_nts++);
    g_ts_fence = atomic_load(&g_fence) + 1;
}
static void ts_report(void) {
    if (!g_nts || atomic_load(&g_done) < g_ts_fence) return;
    uint64_t t[64]; if (vkGetQueryPoolResults(g_dev, g_tsq, 0, g_nts, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) { g_nts = 0; return; }
    double per = g_props.limits.timestampPeriod / 1e6;
    for (uint32_t i = 1; i < g_nts; i++) VLOG("[gpu] %-44s %6.2f ms", g_ts_name[i], (t[i] - t[i - 1]) * per);
    VLOG("[gpu] frame total %.2f ms over %u marks", (t[g_nts - 1] - t[0]) * per, g_nts);
    g_nts = 0;
}
static int rp_begin(void) {
    if (g_in_rp) return 1;
    if (!S.rt_valid) return 0;
    rp_key k; memset(&k, 0, sizeof k);
    VkImageView v[9]; uint64_t ser[9] = {0}; VkClearValue cv[9]; memset(cv, 0, sizeof cv); uint32_t n = 0, w = 1u << 30, h = 1u << 30;
    vtex *cx[8] = {0}; unsigned clv[8] = {0};
    k.n = (uint32_t)S.rt.n;
    for (int i = 0; i < S.rt.n; i++) {
        vtex *x = S.rt.color[i] ? tex_get(&S.rt_tex[i]) : NULL;
        if (!x) continue;
        unsigned lv = S.rt.has_cview[i] && S.rt.cview[i].set_levels ? S.rt.cview[i].base_level : 0;
        x->gpu = 1; k.c[i] = fmt_of(S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : x->d.format).vk ?: x->f.vk;
        if (x->f.decode || !x->mutable_fmt) k.c[i] = x->f.vk;   // as tex_view: no other format without MUTABLE_FORMAT
        cx[i] = x; clv[i] = lv;
        if (mipd(x->w, lv) < w) w = mipd(x->w, lv); if (mipd(x->h, lv) < h) h = mipd(x->h, lv);
    }
    vtex *dx = S.rt.depth ? tex_get(&S.rt_tex[8]) : NULL; unsigned dlv = 0;
    if (dx && !dx->f.depth) dx = NULL;
    if (dx) { dlv = S.rt.has_dview && S.rt.dview.set_levels ? S.rt.dview.base_level : 0; dx->gpu = 1; k.d = dx->f.vk;
              if (mipd(dx->w, dlv) < w) w = mipd(dx->w, dlv); if (mipd(dx->h, dlv) < h) h = mipd(dx->h, dlv); }
    if (w == 1u << 30) return 0;
    // load ops from the pending clears; the rest of the pending clears are done now, outside the pass
    // (a pending clear covers the whole level and every layer, the load op only the render area of one layer: folded only where those are the same)
#define WHOLE(x, lv) ((x)->layers == 1 && mipd((x)->w, lv) == w && mipd((x)->h, lv) == h)
    for (int i = 0; i < S.rt.n; i++) { pend_t pc; if (cx[i] && WHOLE(cx[i], clv[i]) && pend_take(cx[i]->serial, clv[i], &pc)) { k.load[i] = 1; cv[i] = pc.v; } }
    pend_t dpc; int dclr = dx && WHOLE(dx, dlv) && pend_take(dx->serial, dlv, &dpc);
#undef WHOLE
    if (dclr) { k.load[8] = !!(dpc.asp & VK_IMAGE_ASPECT_DEPTH_BIT); k.load[9] = !!(dpc.asp & VK_IMAGE_ASPECT_STENCIL_BIT); }
    pend_flush();
    // store ops from the lookahead
    for (int i = 0; i < S.rt.n; i++) if (cx[i]) k.store[i] = !look_read(cx[i]->serial, 0, 0);
    if (dx) k.store[8] = k.store[9] = !look_read(dx->serial, 1, 0);
    for (int i = 0; i < S.rt.n; i++) if (cx[i]) { v[n] = tex_view(cx[i], &S.rt_tex[i], &S.rt.cview[i], S.rt.has_cview[i], 1); ser[n] = cx[i]->serial; cv[n] = cv[i]; n++; }
    if (dx) { v[n] = tex_view(dx, &S.rt_tex[8], &S.rt.dview, S.rt.has_dview, 1); ser[n] = dx->serial; cv[n] = dclr ? dpc.v : (VkClearValue){0}; n++; }
    rp_key ck = k; memset(ck.load, 0, sizeof ck.load); memset(ck.store, 0, sizeof ck.store);
    g_rp_compat = rp_get(&ck); g_rp = rp_get(&k); g_rp_w = w; g_rp_h = h;
    VkFramebuffer fb = fb_get(g_rp_compat, v, ser, n, w, h);
    barrier_pass(g_cmd);
    if (g_trace) { char nm[48]; snprintf(nm, sizeof nm, "before pass %ux%u c%llu", w, h, (unsigned long long)(cx[0] ? cx[0]->serial : 0)); ts_mark(nm); }
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g_rp, .framebuffer = fb, .renderArea = {{0, 0}, {w, h}}, .clearValueCount = n, .pClearValues = cv};
    vkCmdBeginRenderPass(g_cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    g_in_rp = 1; g_passes++; g_frame_passes++; g_dyn_dirty = 1; g_bound_pipe = VK_NULL_HANDLE;
    TR("pass %ux%u colour %llu%s%s %llu depth %llu%s%s", w, h, (unsigned long long)(cx[0] ? cx[0]->serial : 0), k.load[0] ? " clear" : "", k.store[0] ? " nostore" : "",
       (unsigned long long)(cx[1] ? cx[1]->serial : 0), (unsigned long long)(dx ? dx->serial : 0), k.load[8] ? " clear" : "", k.store[8] ? " nostore" : "");
    return 1;
}

// ---------- Pipelines ----------
typedef struct {
    shader *vs, *fs; VkRenderPass rp; int depth_zero;
    uint32_t attr[16]; vs_st stream[16];
    blend_st blend[8]; uint32_t color, chmask; uint64_t ds; uint32_t poly; uint8_t prim, restart, discard, ncolor;
} pkey;
static nvn_hmap g_pipes, g_layouts;
static VkPipelineCache g_pcache; static uint64_t g_pipes_made;
static VkPipelineLayout layout_get(shader *vs, shader *fs) {
    shader *k[2] = {vs, fs};
    VkPipelineLayout l = nvn_hm_get(&g_layouts, k);
    if (l) return l;
    VkDescriptorSetLayout s[2] = {vs ? vs->dsl : g_empty_dsl, fs ? fs->dsl : g_empty_dsl};
    VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = s};
    VKC(vkCreatePipelineLayout(g_dev, &ci, NULL, &l));
    nvn_hm_put(&g_layouts, k, l);
    return l;
}
static VkBlendFactor bf(unsigned f) {
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 0x10: case 0x11: case 0x12: case 0x13: RED1("dual-source blend factor %#x: Mali has no dual-source blending", f); return VK_BLEND_FACTOR_ONE;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkPrimitiveTopology topo(int prim) {
    switch (prim) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 2: case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // triangles, and quads expanded to triangles
    }
}
static VkPipeline pipe_get(int prim, VkPipelineLayout layout) {
    pkey k; memset(&k, 0, sizeof k);
    shader *vs = S.pr->vs, *fs = S.pr->fs;
    k.vs = vs; k.fs = fs; k.rp = g_rp_compat; k.depth_zero = g_depth_zero; k.prim = (uint8_t)topo(prim);
    k.restart = (uint8_t)(S.restart && (k.prim == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || k.prim == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN || k.prim == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP));
    k.discard = (uint8_t)S.discard;
    for (int l = 0; l < 16; l++) if (vs->inputs >> l & 1) {
        memcpy(&k.attr[l], &S.attr[l], 4);
        if (S.attr[l].fmt) k.stream[S.attr[l].stream] = S.stream[S.attr[l].stream];
    }
    rp_key *rk = NULL;   // the render pass's formats decide which blend targets exist
    for (uint32_t i = 0; i < g_rps.cap && !rk; i++) if (g_rps.vals[i] == (void *)g_rp_compat) rk = (rp_key *)(g_rps.keys + (size_t)i * g_rps.klen);
    k.ncolor = rk ? (uint8_t)rk->n : 0;
    for (int i = 0; i < k.ncolor; i++) if (S.color.blend_en >> i & 1) k.blend[i] = S.blend[i];
    k.color = S.color.blend_en & ((1u << k.ncolor) - 1); k.chmask = S.chmask.mask & (k.ncolor >= 8 ? ~0u : (1u << (4 * k.ncolor)) - 1);
    ds_st ds = S.ds; if (!rk || !rk->d) memset(&ds, 0, sizeof ds);
    memcpy(&k.ds, &ds, 8); memcpy(&k.poly, &S.poly, 4);
    VkPipeline p = nvn_hm_get(&g_pipes, &k);
    if (p) return p;
    uint64_t t0 = now_ns();
    for (int l = 0; l < 16; l++) if (vs->inputs >> l & 1) {   // integer attributes the shader was translated as float for: shader_facts.tsv needs them (vk_sh_build.py)
        unsigned f = S.attr[l].fmt; static uint64_t seen[256]; static int nseen; uint64_t key = (uint64_t)(uintptr_t)vs ^ (uint64_t)l << 56 ^ (uint64_t)f << 48; int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i] == key;
        if (!known && nseen < 256) { seen[nseen++] = key; VLOG("vertex input %s location %d format %#x", vs->key, l, f); }
    }
    // Vertex input: every location the vertex shader reads; one it reads but the game gave no format reads zeros from binding 15
    VkVertexInputAttributeDescription va[16]; VkVertexInputBindingDescription vb[16]; uint32_t nva = 0, nvb = 0, bound = 0; int zero = 0;
    for (int l = 0; l < 16; l++) if (vs->inputs >> l & 1) {
        va_st a = S.attr[l]; VkFormat f = a.fmt ? vfmt_of((int)a.fmt) : 0;
        if (a.fmt && !f) RED1("vertex attribute format %#x has no Vulkan format", a.fmt);
        if (f && ((vs->int_inputs >> l & 1) != (f == VK_FORMAT_R8G8B8A8_UINT || f == VK_FORMAT_R8_UINT || f == VK_FORMAT_R8G8_UINT || f == VK_FORMAT_R16_UINT || f == VK_FORMAT_R16G16_UINT || f == VK_FORMAT_R16G16B16A16_UINT || f == VK_FORMAT_R32_UINT || f == VK_FORMAT_R32G32_UINT || f == VK_FORMAT_R32G32B32A32_UINT || f == VK_FORMAT_R32G32B32_UINT || f == VK_FORMAT_R8G8B8A8_SINT || f == VK_FORMAT_R16G16B16A16_SINT || f == VK_FORMAT_R32_SINT)))
            VLOG("[RED] shader %s input %d is %s but the attribute format %#x is %s", vs->key, l, vs->int_inputs >> l & 1 ? "integer" : "float", a.fmt, vs->int_inputs >> l & 1 ? "float" : "integer");
        if (!f) { va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)l, 15, VK_FORMAT_R32G32B32A32_SFLOAT, 0}; zero = 1; continue; }
        va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)l, a.stream, f, a.offset};
        if (!(bound >> a.stream & 1)) { bound |= 1u << a.stream; vb[nvb++] = (VkVertexInputBindingDescription){a.stream, S.stream[a.stream].stride, S.stream[a.stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX}; }
    }
    if (zero) vb[nvb++] = (VkVertexInputBindingDescription){15, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = nvb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = nva, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = (VkPrimitiveTopology)k.prim, .primitiveRestartEnable = k.restart};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    poly_st po = S.poly;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .rasterizerDiscardEnable = k.discard, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = (VkCullModeFlags)po.cull, .frontFace = po.front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE, .depthBiasEnable = (po.offset_en & 4) != 0, .lineWidth = 1};
    if (po.mode != NVN_POLYGON_MODE_FILL) RED1("polygon mode %u: Mali has no line / point fill, drawn filled", po.mode);
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkStencilOpState sf = {(VkStencilOp)(ds.fail_f ? ds.fail_f - 1 : 0), (VkStencilOp)(ds.zpass_f ? ds.zpass_f - 1 : 0), (VkStencilOp)(ds.zfail_f ? ds.zfail_f - 1 : 0), (VkCompareOp)(ds.sf_f ? ds.sf_f - 1 : 7), 0xff, 0xff, 0};
    VkStencilOpState sb = {(VkStencilOp)(ds.fail_b ? ds.fail_b - 1 : 0), (VkStencilOp)(ds.zpass_b ? ds.zpass_b - 1 : 0), (VkStencilOp)(ds.zfail_b ? ds.zfail_b - 1 : 0), (VkCompareOp)(ds.sf_b ? ds.sf_b - 1 : 7), 0xff, 0xff, 0};
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = ds.dt, .depthWriteEnable = ds.dt && ds.dw,
        .depthCompareOp = (VkCompareOp)(ds.df ? ds.df - 1 : 1), .stencilTestEnable = ds.st, .front = sf, .back = sb};
    VkPipelineColorBlendAttachmentState cb[8];
    for (int i = 0; i < k.ncolor; i++) {
        blend_st b = k.blend[i];
        if (b.adv) RED1("advanced blend mode %u not implemented", (unsigned)b.adv);
        cb[i] = (VkPipelineColorBlendAttachmentState){(k.color >> i & 1), bf(b.src_rgb), bf(b.dst_rgb), (VkBlendOp)(b.eq_rgb ? b.eq_rgb - 1 : 0), bf(b.src_a), bf(b.dst_a),
                                                      (VkBlendOp)(b.eq_a ? b.eq_a - 1 : 0), (VkColorComponentFlags)(k.chmask >> (4 * i) & 0xf)};
    }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = k.ncolor, .pAttachments = cb};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = sh_module(vs), .pName = "main"},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs ? sh_module(fs) : VK_NULL_HANDLE, .pName = "main"}};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = fs ? 2 : 1, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = layout, .renderPass = g_rp_compat};
    VkResult r = vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &ci, NULL, &p);
    if (r != VK_SUCCESS) { VLOG("[RED] pipeline %s / %s failed: %d", vs->key, fs ? fs->key : "-", r); p = VK_NULL_HANDLE; }
    pkey *kk = malloc(sizeof *kk); *kk = k; nvn_hm_put(&g_pipes, kk, p ? (void *)p : (void *)1); free(kk);
    g_pipes_made++;
    OLOG("pipeline %llu: %s / %s prim %d blend %#x in %.1f ms", (unsigned long long)g_pipes_made, vs->key, fs ? fs->key : "-", prim, k.color, (now_ns() - t0) / 1e6);
    return p;
}

// ---------- Resources of a draw ----------
static VkDescriptorPool dpool_new(void) {
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}};
    VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4096, .poolSizeCount = 2, .pPoolSizes = ps};
    VkDescriptorPool p; VKC(vkCreateDescriptorPool(g_dev, &ci, NULL, &p)); return p;
}
static VkDescriptorSet set_alloc(VkDescriptorSetLayout l) {
    slot_t *s = &g_slot[g_cur_slot];
    for (;;) {
        if (s->curdp == s->ndp) { if (s->ndp == 16) die("[vk] descriptor pools exhausted"); s->dp[s->ndp++] = dpool_new(); }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = s->dp[s->curdp], .descriptorSetCount = 1, .pSetLayouts = &l};
        VkDescriptorSet d; VkResult r = vkAllocateDescriptorSets(g_dev, &ai, &d);
        if (r == VK_SUCCESS) return d;
        s->curdp++;
    }
}
// The uniform block bound at (stage, slot), copied once per binding: only the n bytes the shader reads (DMC binds 4 KiB or more per draw and most shaders read 16 vec4s at most),
//   again (larger) if a later shader reads more of the same binding
static VkDeviceSize ubo_ring(int st, int reg, uint64_t n) {
    if (S.ubo[st][reg].copied >= n) return S.ubo[st][reg].ring;
    S.ubo[st][reg].ring = ring_put((const void *)S.ubo[st][reg].addr, n, 256);
    S.ubo[st][reg].copied = n;
    return S.ubo[st][reg].ring;
}
static VkImageView g_dummy_view; static VkSampler g_dummy_samp;
static VkDescriptorSet stage_set(int st, shader *s) {
    VkWriteDescriptorSet w[16]; VkDescriptorBufferInfo bi[16]; VkDescriptorImageInfo ii[16]; int n = 0;
    VkDescriptorSet set = set_alloc(s->dsl);
    for (int i = 0; i < s->nres; i++) {
        const sres *r = &s->res[i];
        w[n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = r->binding, .descriptorCount = 1};
        switch (r->kind) {
            case RK_SUP: bi[n] = (VkDescriptorBufferInfo){g_supbuf, 0, 65536}; break;
            case RK_C1: bi[n] = (VkDescriptorBufferInfo){g_c1buf, s->c1_off, 65536}; break;
            case RK_UBO: {
                uint64_t sz = S.ubo[st][r->reg & 15].size; if (sz > 65536) sz = 65536; if (r->need && r->need < sz) sz = r->need;
                if (sz && S.ubo[st][r->reg & 15].addr) bi[n] = (VkDescriptorBufferInfo){g_ring, ubo_ring(st, r->reg & 15, sz), sz};
                else bi[n] = (VkDescriptorBufferInfo){g_zerobuf, 0, 65536};
                break; }
            case RK_TEX: {
                uint64_t h = r->reg < 32 ? S.tex[st][r->reg] : 0; int tid = (int)(h & 0xfffff), sid = (int)(h >> 20 & 0xfff);
                VkImageView v = g_dummy_view; VkSampler sp = g_dummy_samp;
                if (tid && tid < g_ntreg && g_treg[tid].live) {
                    treg_t *tr = &g_treg[tid]; vtex *x = tex_get(&tr->t);
                    if (x) { tex_upload(x); v = tex_view(x, &tr->t, &tr->v, tr->has_v, 0); }
                }
                if (sid && sid < g_nsreg && g_sreg[sid].live) { if (!g_sreg[sid].s) g_sreg[sid].s = samp_get(&g_sreg[sid].p); sp = g_sreg[sid].s; }
                ii[n] = (VkDescriptorImageInfo){sp, v, VK_IMAGE_LAYOUT_GENERAL};
                w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &ii[n]; n++;
                continue; }
        }
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[n].pBufferInfo = &bi[n]; n++;
    }
    vkUpdateDescriptorSets(g_dev, (uint32_t)n, w, 0, NULL);
    return set;
}

// Vertex data of a draw: the indexed range [lo, hi] of each stream the pipeline reads, copied to the ring
typedef struct { uint64_t addr, len; VkDeviceSize off; } vcopy;
enum { NVC = 1024 };
static vcopy g_vc[NVC];   // this submission's copies, by (addr, len) hash
static VkDeviceSize vcopy_get(uint64_t addr, uint64_t len) {
    uint32_t h = (uint32_t)(nvn_hm_hash(&addr, 8) ^ len * 0x9e3779b1u) & (NVC - 1);
    for (int i = 0; i < 4; i++) { vcopy *c = &g_vc[(h + i) & (NVC - 1)]; if (c->addr == addr && c->len == len) return c->off; }
    VkDeviceSize off = ring_put((const void *)addr, len, 16);
    g_vc[h] = (vcopy){addr, len, off};
    return off;
}
static void index_range(const void *ib, int isz, int32_t n, uint32_t *lo, uint32_t *hi, int restart, uint32_t ri) {
    uint32_t mn = UINT32_MAX, mx = 0;
    if (isz < 4) ri &= isz == 2 ? 0xffffu : 0xffu;   // the restart index as the index buffer's width holds it
    if (isz == 4) { const uint32_t *p = ib; for (int32_t i = 0; i < n; i++) { uint32_t v = p[i]; if (restart && v == ri) continue; if (v < mn) mn = v; if (v > mx) mx = v; } }
    else if (isz == 2) { const uint16_t *p = ib; for (int32_t i = 0; i < n; i++) { uint32_t v = p[i]; if (restart && v == ri) continue; if (v < mn) mn = v; if (v > mx) mx = v; } }
    else { const uint8_t *p = ib; for (int32_t i = 0; i < n; i++) { uint32_t v = p[i]; if (restart && v == ri) continue; if (v < mn) mn = v; if (v > mx) mx = v; } }
    if (mn > mx) mn = mx = 0;
    *lo = mn; *hi = mx;
}
static void bind_vertices(int64_t vlo, int64_t vhi) {
    shader *vs = S.pr->vs; uint32_t done = 0;
    for (int l = 0; l < 16; l++) if (vs->inputs >> l & 1) {
        va_st a = S.attr[l];
        if (!a.fmt) { VkBuffer b = g_zerobuf; VkDeviceSize o = 0; vkCmdBindVertexBuffers(g_cmd, 15, 1, &b, &o); continue; }
        unsigned s = a.stream; if (done >> s & 1) continue; done |= 1u << s;
        uint64_t stride = S.stream[s].stride, base = S.vbuf[s].address, size = S.vbuf[s].size, ext = 0;
        for (int m = 0; m < 16; m++) if ((vs->inputs >> m & 1) && S.attr[m].fmt && S.attr[m].stream == s) {   // the bytes the stream's attributes reach in a vertex
            int bpb = nvn_fmt_bpb((int)S.attr[m].fmt, NULL, NULL); uint64_t e = S.attr[m].offset + (uint64_t)(bpb ? bpb : 16); if (e > ext) ext = e; }
        uint64_t from = S.stream[s].divisor || !stride ? 0 : (uint64_t)vlo * stride, to = S.stream[s].divisor ? size : (uint64_t)vhi * stride + ext;
        if (to > size) to = size;
        if (!base || from >= to) { VkBuffer b = g_zerobuf; VkDeviceSize o = 0; vkCmdBindVertexBuffers(g_cmd, s, 1, &b, &o); continue; }
        VkDeviceSize off = vcopy_get(base + from, to - from);
        VkBuffer b = g_ring; vkCmdBindVertexBuffers(g_cmd, s, 1, &b, &off);
    }
}
static void apply_dynamic(void) {
    if (!g_dyn_dirty) return;
    float x = S.vp[0][0], y = S.vp[0][1], w = S.vp[0][2], h = S.vp[0][3];
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = (float)g_rp_w; h = (float)g_rp_h; }
    float dn = S.dnear < 0 ? 0 : S.dnear > 1 ? 1 : S.dnear, df = S.dfar < 0 ? 0 : S.dfar > 1 ? 1 : S.dfar;
    VkViewport v = {x, y + h, w, -h, dn, df};   // y up in clip space (the shaders' Metal convention), origin upper left
    vkCmdSetViewport(g_cmd, 0, 1, &v);
    int32_t sx = S.sc.x < 0 ? 0 : S.sc.x, sy = S.sc.y < 0 ? 0 : S.sc.y;
    int64_t sw = (int64_t)S.sc.w - (sx - S.sc.x), sh = (int64_t)S.sc.h - (sy - S.sc.y);
    if (sx + sw > g_rp_w) sw = (int64_t)g_rp_w - sx; if (sy + sh > g_rp_h) sh = (int64_t)g_rp_h - sy;
    VkRect2D sc = {{sx, sy}, {(uint32_t)(sw > 0 ? sw : 0), (uint32_t)(sh > 0 ? sh : 0)}};
    vkCmdSetScissor(g_cmd, 0, 1, &sc);
    vkCmdSetDepthBias(g_cmd, S.bias[0], S.bias[1], S.bias[2]);
    vkCmdSetBlendConstants(g_cmd, S.blendc);
    for (int f = 0; f < 2; f++) {
        VkStencilFaceFlags fl = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilCompareMask(g_cmd, fl, S.st_vmask[f]); vkCmdSetStencilWriteMask(g_cmd, fl, S.st_wmask[f]); vkCmdSetStencilReference(g_cmd, fl, S.st_ref[f]);
    }
    g_dyn_dirty = 0;
}
static void draw(int prim, int indexed, const p_draw *dw, const p_draw_idx *di) {
    if (!S.pr || !S.pr->vs) { RED1("draw without a vertex shader"); return; }
    if ((indexed ? di->ind : dw->ind) || (indexed ? di->mdi_cnt : dw->mdi_cnt)) { RED1("indirect draws not implemented"); return; }
    if (!rp_begin()) return;
    VkPipelineLayout layout = layout_get(S.pr->vs, S.pr->fs);
    VkPipeline p = pipe_get(prim, layout);
    if (!p || p == (VkPipeline)1) return;
    if (p != g_bound_pipe) { vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g_bound_pipe = p; }
    if (layout != g_bound_layout) { S.dirty_set[0] = S.dirty_set[1] = 1; g_bound_layout = layout; }
    apply_dynamic();
    for (int st = 0; st < 2; st++) {
        shader *s = st ? S.pr->fs : S.pr->vs;
        if (!s || !S.dirty_set[st]) continue;
        g_set[st] = stage_set(st, s); S.dirty_set[st] = 0;
        vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, (uint32_t)st, 1, &g_set[st], 0, NULL);
    }
    int inst = indexed ? (di->instances > 0 ? di->instances : 1) : (dw->instances > 0 ? dw->instances : 1);
    if (indexed) {
        int isz = di->itype == NVN_INDEX_TYPE_UNSIGNED_INT ? 4 : di->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 1;
        if (!di->ibuf || di->count <= 0) return;
        uint32_t lo, hi; index_range((const void *)di->ibuf, isz, di->count, &lo, &hi, S.restart, S.restart_index);
        int64_t vlo = (int64_t)lo + di->base_vertex, vhi = (int64_t)hi + di->base_vertex; if (vlo < 0) vlo = 0;
        bind_vertices(vlo, vhi);
        uint32_t count = (uint32_t)di->count; VkDeviceSize ioff; VkIndexType it;
        if (prim == NVN_DRAW_PRIMITIVE_QUADS || prim == NVN_DRAW_PRIMITIVE_LINE_LOOP) {
            if (prim == NVN_DRAW_PRIMITIVE_LINE_LOOP) RED1("line loops drawn as strips");
            uint32_t *tmp = malloc((size_t)count * 6 / 4 * 4 + 16);
            count = prim == NVN_DRAW_PRIMITIVE_QUADS ? nvn_expand_primitive(tmp, (const uint8_t *)di->ibuf, count, (unsigned)isz, 7, S.restart, S.restart_index) : count;
            if (prim == NVN_DRAW_PRIMITIVE_LINE_LOOP) for (uint32_t i = 0; i < count; i++) tmp[i] = isz == 4 ? ((const uint32_t *)di->ibuf)[i] : isz == 2 ? ((const uint16_t *)di->ibuf)[i] : ((const uint8_t *)di->ibuf)[i];
            ioff = ring_put(tmp, (size_t)count * 4, 4); it = VK_INDEX_TYPE_UINT32; free(tmp);
        } else if (isz == 1) {   // Vulkan 1.1 has no 8-bit indices without VK_EXT_index_type_uint8: widened
            uint16_t *tmp = malloc((size_t)count * 2); for (uint32_t i = 0; i < count; i++) { uint8_t v = ((const uint8_t *)di->ibuf)[i]; tmp[i] = S.restart && v == S.restart_index ? 0xffff : v; }
            ioff = ring_put(tmp, (size_t)count * 2, 4); it = VK_INDEX_TYPE_UINT16; free(tmp);
        } else {
            ioff = vcopy_get(di->ibuf, (uint64_t)count * isz); it = isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
            if (S.restart && S.restart_index != (isz == 4 ? 0xffffffffu : 0xffffu)) RED1("primitive restart index %#x is not all ones", S.restart_index);
        }
        vkCmdBindIndexBuffer(g_cmd, g_ring, ioff, it);
        vkCmdDrawIndexed(g_cmd, count, (uint32_t)inst, 0, (int32_t)(di->base_vertex - vlo), (uint32_t)di->base_instance);
    } else {
        if (dw->count <= 0) return;
        bind_vertices(dw->first, (int64_t)dw->first + dw->count - 1);
        if (prim == NVN_DRAW_PRIMITIVE_QUADS) {
            uint32_t n = (uint32_t)dw->count / 4 * 6, *tmp = malloc((size_t)n * 4 + 4);
            for (uint32_t q = 0; q < (uint32_t)dw->count / 4; q++) { uint32_t b = q * 4; uint32_t *o = tmp + q * 6; o[0] = b; o[1] = b + 1; o[2] = b + 2; o[3] = b; o[4] = b + 2; o[5] = b + 3; }
            VkDeviceSize ioff = ring_put(tmp, (size_t)n * 4, 4); free(tmp);
            vkCmdBindIndexBuffer(g_cmd, g_ring, ioff, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(g_cmd, n, (uint32_t)inst, 0, 0, (uint32_t)dw->base_instance);
        } else vkCmdDraw(g_cmd, (uint32_t)dw->count, (uint32_t)inst, 0, (uint32_t)dw->base_instance);
    }
    g_draws++; g_frame_draws++; g_pass_draws++;
    if (g_trace > 1) {
        char tx[64] = ""; for (int st = 0; st < 2; st++) { shader *sh = st ? S.pr->fs : S.pr->vs; for (int i = 0; sh && i < sh->nres; i++) if (sh->res[i].kind == RK_TEX) { uint64_t hh = S.tex[st][sh->res[i].reg & 31];
            snprintf(tx + strlen(tx), sizeof tx - strlen(tx), " t%d=%llu", sh->res[i].reg, (unsigned long long)((hh & 0xfffff) < (uint64_t)g_ntreg ? g_treg[hh & 0xfffff].t.serial : 0)); } }
        TR("draw %s/%s prim %d count %d vp %.0f,%.0f %.0fx%.0f sc %d,%d %dx%d blend %#x ds %d%d%d cull %u%s", S.pr->vs->key, S.pr->fs ? S.pr->fs->key : "-", prim, indexed ? di->count : dw->count,
           S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.sc.x, S.sc.y, S.sc.w, S.sc.h, S.color.blend_en, S.ds.dt, S.ds.dw, (int)S.ds.df, (unsigned)S.poly.cull, tx);
    }
}

// ---------- Clears and copies ----------
static int rt_index_of(uint64_t serial) {   // attachment slot of a texture in the current targets (8 = depth), -1 = not bound
    for (int i = 0; i < S.rt.n; i++) if (S.rt.color[i] && S.rt_tex[i].serial == serial) return i;
    if (S.rt.depth && S.rt_tex[8].serial == serial) return 8;
    return -1;
}
static void clear_color_att(int i, const float *rgba, int mask, const NVNcopyRegion *r) {
    if (!rp_begin()) return;
    if ((mask & 0xf) != 0xf) RED1("clear with channel mask %#x cleared all channels", mask);
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)i, {.color = {{rgba[0], rgba[1], rgba[2], rgba[3]}}}};   // colorAttachment indexes the subpass's references, which follow the target slots
    VkClearRect cr = {{{0, 0}, {g_rp_w, g_rp_h}}, 0, 1};
    if (r) { int32_t x = r->xoffset < 0 ? 0 : r->xoffset, y = r->yoffset < 0 ? 0 : r->yoffset; uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height;
             if (x + w > g_rp_w) w = g_rp_w - (uint32_t)x; if (y + h > g_rp_h) h = g_rp_h - (uint32_t)y; cr.rect = (VkRect2D){{x, y}, {w, h}}; }
    if (!cr.rect.extent.width || !cr.rect.extent.height) return;
    vkCmdClearAttachments(g_cmd, 1, &ca, 1, &cr);
}
static void clear_tex(const p_clear_tex *c) {
    if (!c->tex) return;
    TR("clear texture %llu %d,%d %dx%d mask %#x (%.2f %.2f %.2f %.2f)", (unsigned long long)c->tex->serial, c->r.xoffset, c->r.yoffset, c->r.width, c->r.height, c->mask, c->c.f[0], c->c.f[1], c->c.f[2], c->c.f[3]);
    int i = rt_index_of(c->tex->serial);
    vtex *x = tex_get(c->tex); if (!x) return;
    x->gpu = 1;
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0;
    int whole = c->r.xoffset == 0 && c->r.yoffset == 0 && (uint32_t)c->r.width >= mipd(x->w, lv) && (uint32_t)c->r.height >= mipd(x->h, lv);
    float f[4]; for (int k = 0; k < 4; k++) f[k] = c->is_uint ? (float)c->c.u[k] : c->c.f[k];
    if (i >= 0 && i < 8 && g_in_rp) {
        int rlv = S.rt.has_cview[i] && S.rt.cview[i].set_levels ? S.rt.cview[i].base_level : 0;
        if ((unsigned)rlv == lv) { clear_color_att(i, f, c->mask, whole ? NULL : &c->r); return; }
    }
    if (whole && (c->mask & 0xf) == 0xf && !(c->has_view && c->view.set_layers && x->layers > 1)) {
        rp_end();   // not an attachment of the open pass: it ends, and the clear waits for the next pass
        VkClearValue v; if (c->is_uint) memcpy(v.color.uint32, c->c.u, 16); else memcpy(v.color.float32, c->c.f, 16);
        if (x->f.depth) { v.depthStencil.depth = f[0]; v.depthStencil.stencil = (uint32_t)f[1]; }
        pend_add(x, lv, x->f.depth ? x->aspect : VK_IMAGE_ASPECT_COLOR_BIT, v); return;
    }
    rp_end(); pend_flush(); barrier_pass(g_cmd);
    VkImageSubresourceRange sr = {x->aspect, lv, 1, c->has_view && c->view.set_layers ? c->view.min_layer : 0, 1};
    if (x->f.depth) { VkClearDepthStencilValue z = {f[0], (uint32_t)f[1]}; vkCmdClearDepthStencilImage(g_cmd, x->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &sr); }
    else {
        if (!whole) RED1("partial texture clear outside a render pass cleared the whole level");
        VkClearColorValue v; if (c->is_uint) memcpy(v.uint32, c->c.u, 16); else memcpy(v.float32, c->c.f, 16);
        vkCmdClearColorImage(g_cmd, x->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &sr);
    }
}
static void clear_ds(const p_clear_ds *c) {
    if (!S.rt.depth) return;
    vtex *x = tex_get(&S.rt_tex[8]); if (!x || !x->f.depth) return;
    x->gpu = 1;
    VkImageAspectFlags a = 0;
    if (c->depth_mask) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (c->stencil_mask && (x->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!a) return;
    if ((c->stencil_mask & 0xff) != 0xff && (a & VK_IMAGE_ASPECT_STENCIL_BIT)) RED1("stencil clear with write mask %#x cleared all bits", c->stencil_mask);
    if (!g_in_rp) { pend_add(x, S.rt.has_dview && S.rt.dview.set_levels ? S.rt.dview.base_level : 0, a, (VkClearValue){.depthStencil = {c->depth, (uint32_t)c->stencil}}); return; }
    VkClearAttachment ca = {a, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect cr = {{{0, 0}, {g_rp_w, g_rp_h}}, 0, 1};
    vkCmdClearAttachments(g_cmd, 1, &ca, 1, &cr);
}
static void copy_t2t(const p_copy_t2t *c) {
    TR("copy %llu %d,%d %dx%d → %llu %d,%d %dx%d flags %#x", (unsigned long long)(c->src ? c->src->serial : 0), c->sr.xoffset, c->sr.yoffset, c->sr.width, c->sr.height,
       (unsigned long long)(c->dst ? c->dst->serial : 0), c->dr.xoffset, c->dr.yoffset, c->dr.width, c->dr.height, c->flags);
    if (c->flags & DM_COPY_RESOLVE) RED1("multisample resolve copied as a plain copy");
    vtex *s = tex_get(c->src), *d = tex_get(c->dst);
    if (!s || !d) return;
    tex_upload(s); d->gpu = 1;
    rp_end(); pend_flush(); barrier_pass(g_cmd);
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    unsigned sz = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, dz = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    int mx = c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT, my = c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT;
    VkImageBlit b = {{s->aspect, sl, sz + (unsigned)c->sr.zoffset, 1}, {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, 1}},
                     {d->aspect, dl, dz + (unsigned)c->dr.zoffset, 1}, {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, 1}}};
    if (mx) { int32_t t = b.dstOffsets[0].x; b.dstOffsets[0].x = b.dstOffsets[1].x; b.dstOffsets[1].x = t; }
    if (my) { int32_t t = b.dstOffsets[0].y; b.dstOffsets[0].y = b.dstOffsets[1].y; b.dstOffsets[1].y = t; }
    if (s->f.vk == d->f.vk && c->sr.width == c->dr.width && c->sr.height == c->dr.height && !mx && !my) {
        VkImageCopy ic = {b.srcSubresource, b.srcOffsets[0], b.dstSubresource, b.dstOffsets[0], {(uint32_t)c->sr.width, (uint32_t)c->sr.height, 1}};
        vkCmdCopyImage(g_cmd, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else if (s->f.depth || d->f.depth) RED1("scaling / converting depth copy not implemented");
    else vkCmdBlitImage(g_cmd, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ---------- Replay ----------
static void buf_written(uint64_t d, uint64_t n) {   // the replay wrote game memory: copies made of it this submission are stale
    for (int st = 0; st < 2; st++) for (int r = 0; r < 16; r++) if (S.ubo[st][r].addr < d + n && d < S.ubo[st][r].addr + S.ubo[st][r].size) { S.ubo[st][r].copied = 0; S.dirty_set[st] = 1; }
    for (int i = 0; i < NVC; i++) if (g_vc[i].len && g_vc[i].addr < d + n && d < g_vc[i].addr + g_vc[i].len) g_vc[i].len = 0, g_vc[i].addr = 0;
}
static void walk(const uint8_t *p, int depth) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const void *a = p + 8;
        g_wstack[depth] = p + h.size; g_wdepth = depth;   // where the lookahead starts: after this record, then after each enclosing call
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (depth < 8) { walk((const uint8_t *)t, depth + 1); g_wdepth = depth; } break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4);
                post_add((post_t){1, val, m, v, 0, 0}); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); if (ad) post_add((post_t){2, NULL, 0, 0, ad, t}); break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_cur_job; s->fenced = 1; break; }   // the job number, the fence space mtl_wait takes
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8); if (s && d) { memmove((void *)d, (const void *)s, n); buf_written(d, n); } break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8); if (d) { memcpy((void *)d, (const uint8_t *)a + 16, n); buf_written(d, n); } break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = a; uint32_t *d = (uint32_t *)cb->addr; for (uint64_t i = 0; d && i < cb->size / 4; i++) d[i] = cb->value; if (d) buf_written(cb->addr, cb->size); break; }
            case OP_SET_RT: { const p_set_rt *r = a;
                rp_end();
                memcpy(&S.rt, r, sizeof *r); S.rt_valid = 1;
                for (int i = 0; i < r->n && i < 8; i++) if (r->color[i]) S.rt_tex[i] = *r->color[i];
                if (r->depth) S.rt_tex[8] = *r->depth;
                break; }
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; if (c->index >= 0 && c->index < S.rt.n && S.rt.color[c->index]) { vtex *x = tex_get(&S.rt_tex[c->index]); if (!x) break; x->gpu = 1;
                if (!g_in_rp && (c->mask & 0xf) == 0xf) { VkClearValue v; memcpy(v.color.float32, c->rgba, 16); pend_add(x, S.rt.has_cview[c->index] && S.rt.cview[c->index].set_levels ? S.rt.cview[c->index].base_level : 0, VK_IMAGE_ASPECT_COLOR_BIT, v); }
                else clear_color_att(c->index, c->rgba, c->mask, NULL); } break; }
            case OP_CLEAR_DS: clear_ds(a); break;
            case OP_CLEAR_TEX: clear_tex(a); break;
            case OP_VIEWPORTS: { const p_viewports *v = a; for (int i = 0; i < v->n && v->first + i < 16; i++) memcpy(S.vp[v->first + i], v->v[i], 16); g_dyn_dirty = 1; break; }
            case OP_SCISSOR: memcpy(&S.sc, a, sizeof S.sc); g_dyn_dirty = 1; break;
            case OP_DEPTH_RANGE: { const p_depth_range *d = a; S.dnear = d->n; S.dfar = d->f; g_dyn_dirty = 1; break; }
            case OP_BIND_PROGRAM: { const p_bind_program *b = a; prog_res *r = b->prog ? prog_get(b->prog) : NULL;
                if (b->stages & NVN_SHADER_STAGE_VERTEX_BIT) S.cur.vs = r ? r->vs : NULL;
                if (b->stages & NVN_SHADER_STAGE_FRAGMENT_BIT) S.cur.fs = r ? r->fs : NULL;
                S.pr = &S.cur; S.dirty_set[0] = S.dirty_set[1] = 1;
                if (g_trace > 1) TR("bind program %llu stages %#x (its stages: vs %s fs %s, %u)", (unsigned long long)(b->prog ? b->prog->serial : 0), b->stages, r && r->vs ? r->vs->key : "-", r && r->fs ? r->fs->key : "-", b->prog ? b->prog->nstage : 0);
                break; }
            case OP_BIND_STATE: { const p_bind_state *b = a;
                switch (b->kind) {
                    case 0: { blend_st s; memcpy(&s, b->bytes, sizeof s); S.blend[s.target & 7] = s; break; }
                    case 1: memcpy(&S.color, b->bytes, sizeof S.color); break;
                    case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                    case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break;
                    case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break;
                    case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
                }
                break; }
            case OP_BIND_VATTR: { const p_bind_vattr *v = a; for (int i = 0; i < v->n && v->first + i < 16; i++) memcpy(&S.attr[v->first + i], &v->st[i], 4); for (int i = v->first + v->n; i < 16; i++) memset(&S.attr[i], 0, 4); break; }
            case OP_BIND_VSTREAM: { const p_bind_vstream *v = a; for (int i = 0; i < v->n && v->first + i < 16; i++) S.stream[v->first + i] = v->st[i]; break; }
            case OP_BIND_VBUF: { const p_bind_vbuf *v = a; for (int i = 0; i < v->n && v->first + i < 16; i++) S.vbuf[v->first + i] = v->r[i]; break; }
            case OP_BIND_UBO: { const p_bind_buf *b = a; int st = b->stage == NVN_SHADER_STAGE_FRAGMENT ? 1 : b->stage == NVN_SHADER_STAGE_VERTEX ? 0 : -1;
                if (st < 0) { RED1("uniform buffer bound to stage %d", b->stage); break; }
                if (b->index >= 0 && b->index < 16) { S.ubo[st][b->index].addr = b->addr; S.ubo[st][b->index].size = b->size; S.ubo[st][b->index].copied = 0; S.dirty_set[st] = 1; }
                break; }
            case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t; const p_bind_handles *b = bind_rec(h.op, a, &t);
                int st = b->stage == NVN_SHADER_STAGE_FRAGMENT ? 1 : b->stage == NVN_SHADER_STAGE_VERTEX ? 0 : -1;
                if (st < 0) break;
                if (b->defer) RED1("deferred texture binds not implemented");
                if (h.op == OP_BIND_TEX) RED1("separate texture binds not implemented");
                for (int i = 0; i < b->n; i++) if (b->first + i < 32) S.tex[st][b->first + i] = b->h[i];
                S.dirty_set[st] = 1; break; }
            case OP_BIND_SAMP: RED1("separate sampler binds not implemented"); break;
            case OP_BIND_SSBO: RED1("storage buffers not implemented"); break;
            case OP_BIND_IMG: RED1("images not implemented"); break;
            case OP_STENCIL: { const p_stencil *s = a;
                for (int f = 0; f < 2; f++) if (s->face & (1 << f)) { if (s->which == 0) S.st_vmask[f] = (uint32_t)s->value_mask; else if (s->which == 1) S.st_wmask[f] = (uint32_t)s->mask; else S.st_ref[f] = (uint32_t)s->ref; }
                g_dyn_dirty = 1; break; }
            case OP_POLY_OFFSET: { const p_poly_offset *o = a; S.bias[0] = o->units; S.bias[1] = o->clamp; S.bias[2] = o->factor; g_dyn_dirty = 1; break; }
            case OP_BLEND_COLOR: memcpy(S.blendc, ((const p_blend_color *)a)->rgba, 16); g_dyn_dirty = 1; break;
            case OP_PRIM_RESTART: { const p_prim_restart *r = a; S.restart = (int)r->on; S.restart_index = r->index; break; }
            case OP_RAST_DISCARD: S.discard = ((const p_rast_discard *)a)->on != 0; break;
            case OP_DEPTH_CLAMP: if (((const p_depth_clamp *)a)->on) RED1("depth clamp: Mali has no depthClamp, depth is clipped"); break;
            case OP_DRAW: { p_draw t; const p_draw *d = draw_rec(a, h.size, &t); draw(d->prim, 0, d, NULL); break; }
            case OP_DRAW_IDX: { p_draw_idx t; const p_draw_idx *d = draw_idx_rec(a, h.size, &t); draw(d->prim, 1, NULL, d); break; }
            case OP_COPY_T2T: copy_t2t(a); break;
            case OP_BARRIER: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_COPY_STRIDE: break;
            case OP_DISPATCH: RED1("compute dispatch not implemented"); break;
            case OP_COPY_B2T: RED1("buffer → texture copy not implemented"); break;
            case OP_COPY_T2B: RED1("texture → buffer copy not implemented"); break;
            case OP_RENDER_COND: RED1("conditional rendering not implemented"); break;
            case OP_VP_SWIZZLE: RED1("viewport swizzle not implemented"); break;
            default: RED1("op %u not implemented", h.op); break;
        }
        p += h.size;
    }
}

// ---------- Init ----------
static void init_locked(void) {
    if (g_dev) return;
    g_log = getenv("DM_VK_LOG") != NULL;
    int win = &dm_android_window != NULL;   // the app build (activity.c defines the window variable): surfaces may come and go, so the extensions are always on
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dm", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = win ? 2 : 0, .ppEnabledExtensionNames = iext};
    VKC(vkCreateInstance(&ici, NULL, &g_inst));
    uint32_t n = 1; VkResult er = vkEnumeratePhysicalDevices(g_inst, &n, &g_pd); if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !n) die("[vk] no physical device");
    vkGetPhysicalDeviceProperties(g_pd, &g_props); vkGetPhysicalDeviceMemoryProperties(g_pd, &g_mp);
    VkQueueFamilyProperties qf[8]; uint32_t nq = 8; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qf);
    for (g_qf = 0; g_qf < nq && !(qf[g_qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); g_qf++) {}
    float prio = 1; VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qf, .queueCount = 1, .pQueuePriorities = &prio};
    const char *dext[] = {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR tf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR, .timelineSemaphore = VK_TRUE};
    VkPhysicalDeviceFeatures feat = {.samplerAnisotropy = VK_TRUE, .independentBlend = VK_TRUE, .fullDrawIndexUint32 = VK_TRUE};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &tf, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = win ? 3 : 2,
                              .ppEnabledExtensionNames = dext, .pEnabledFeatures = &feat};
    VKC(vkCreateDevice(g_pd, &dci, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);
    p_wait_sems = (PFN_vkWaitSemaphoresKHR)vkGetDeviceProcAddr(g_dev, "vkWaitSemaphoresKHR");
    p_sem_value = (PFN_vkGetSemaphoreCounterValueKHR)vkGetDeviceProcAddr(g_dev, "vkGetSemaphoreCounterValueKHR");
    if (!p_wait_sems || !p_sem_value) die("[vk] no timeline semaphore functions");
    VkSemaphoreTypeCreateInfoKHR sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO_KHR, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE_KHR, .initialValue = 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &sti};
    VKC(vkCreateSemaphore(g_dev, &sci, NULL, &g_tl));
    g_ring = buf_new(RING_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g_ring_mem, (void **)&g_ring_map);
    for (int i = 0; i < NSLOT; i++) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = g_qf};
        VKC(vkCreateCommandPool(g_dev, &pci, NULL, &g_slot[i].pool));
        VkCommandBuffer cb[2]; VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_slot[i].pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        VKC(vkAllocateCommandBuffers(g_dev, &cai, cb)); g_slot[i].pre = cb[0]; g_slot[i].cmd = cb[1];
    }
    VkDescriptorSetLayoutCreateInfo eli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VKC(vkCreateDescriptorSetLayout(g_dev, &eli, NULL, &g_empty_dsl));
    VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VKC(vkCreatePipelineCache(g_dev, &pcci, NULL, &g_pcache));
    nvn_hm_init(&g_tex, 8); nvn_hm_init(&g_progs, 8); nvn_hm_init(&g_samp_cache, sizeof(samp_par)); nvn_hm_init(&g_rps, sizeof(rp_key));
    nvn_hm_init(&g_pipes, sizeof(pkey)); nvn_hm_init(&g_layouts, 2 * sizeof(void *));
    shaders_load();
    {   // a 1x1 zero texture and a nearest sampler for slots the game left empty
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkImage im; VKC(vkCreateImage(g_dev, &ci, NULL, &im)); VkMemoryRequirements r; vkGetImageMemoryRequirements(g_dev, im, &r);
        VkDeviceMemory m = mem_alloc(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); VKC(vkBindImageMemory(g_dev, im, m, 0));
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKC(vkCreateImageView(g_dev, &vi, NULL, &g_dummy_view));
        samp_par sp; memset(&sp, 0, sizeof sp); g_dummy_samp = samp_get(&sp);
        // its layout and contents are set by the first submission's pre-pass
        VkCommandBuffer cb = g_slot[0].pre; VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKC(vkBeginCommandBuffer(cb, &bi));
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkClearColorValue z = {{0}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
        VKC(vkEndCommandBuffer(cb));
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        VKC(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); VKC(vkQueueWaitIdle(g_q)); vkResetCommandPool(g_dev, g_slot[0].pool, 0);
    }
    pthread_t th; if (pthread_create(&th, NULL, completion_thread, NULL)) die("[vk] completion thread"); pthread_detach(th);
    VLOG("device %s (Vulkan %u.%u.%u), %s", g_props.deviceName, VK_VERSION_MAJOR(g_props.apiVersion), VK_VERSION_MINOR(g_props.apiVersion), VK_VERSION_PATCH(g_props.apiVersion),
         win ? "presenting to the window" : "no window (offscreen)");
}
void mtl_init(void) { pthread_mutex_lock(&g_mu); init_locked(); pthread_mutex_unlock(&g_mu); }

// ---------- Submission ----------
static void sub_begin(void) {
    pthread_mutex_lock(&g_open_mu);
    pthread_mutex_lock(&g_mu);
    init_locked();
    dead_reap();
    g_sub_n++;
    g_cur_slot = (g_cur_slot + 1) % NSLOT;
    slot_t *s = &g_slot[g_cur_slot];
    if (s->fence && atomic_load(&g_done) < s->fence) { uint64_t t = now_ns(); if (!vk_wait(s->fence, UINT64_MAX)) die("[vk] slot wait"); g_wait_ns[1] += now_ns() - t; g_wait_n[1]++; }
    vkResetCommandPool(g_dev, s->pool, 0);
    for (int i = 0; i < s->ndp; i++) vkResetDescriptorPool(g_dev, s->dp[i], 0);
    s->curdp = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKC(vkBeginCommandBuffer(s->pre, &bi)); VKC(vkBeginCommandBuffer(s->cmd, &bi));
    g_pre = s->pre; g_cmd = s->cmd; g_pre_used = 0; g_pre_war = 0; g_rec = 1;
    memset(g_vc, 0, sizeof g_vc);
    // NVN's state is the GPU's and outlives a submission (programs, textures, targets bound in one command buffer stay bound for the next): kept, only what this backend
    // built from it for the previous submission is redone (uniform copies lived in that submission's ring space, descriptor sets in its slot's pools, bindings in its command buffer)
    { static int first = 1; if (first) { state_reset(); first = 0; } }
    for (int st = 0; st < 2; st++) { for (int r = 0; r < 16; r++) S.ubo[st][r].copied = 0; S.dirty_set[st] = 1; }
    g_dyn_dirty = 1; g_bound_pipe = VK_NULL_HANDLE; g_bound_layout = VK_NULL_HANDLE; g_in_rp = 0;
    pthread_mutex_unlock(&g_mu);
}
static void exec_handle(const uint8_t *handle) { if (!handle) return; uint64_t t0 = now_ns(); pthread_mutex_lock(&g_mu); walk(handle, 0); pthread_mutex_unlock(&g_mu); atomic_fetch_add(&g_replay_ns, now_ns() - t0); }
static void submit_locked(VkSemaphore wait_bin, VkSemaphore signal_bin) {   // ends and submits the open submission as fence g_fence + 1
    rp_end(); pend_flush();
    // the pre-pass's uploads are visible to everything after them
    if (g_pre_used) barrier_full(g_pre);
    VKC(vkEndCommandBuffer(g_pre)); VKC(vkEndCommandBuffer(g_cmd));
    uint64_t v = atomic_load(&g_fence) + 1;
    VkCommandBuffer cbs[2] = {g_pre, g_cmd};
    uint64_t sig[2] = {v, 0}, waitv = 0;
    VkSemaphore sigs[2] = {g_tl, signal_bin};
    VkTimelineSemaphoreSubmitInfoKHR ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR, .waitSemaphoreValueCount = wait_bin ? 1 : 0, .pWaitSemaphoreValues = &waitv,
                                           .signalSemaphoreValueCount = signal_bin ? 2 : 1, .pSignalSemaphoreValues = sig};
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .waitSemaphoreCount = wait_bin ? 1 : 0, .pWaitSemaphores = &wait_bin, .pWaitDstStageMask = &ws,
                       .commandBufferCount = 2, .pCommandBuffers = cbs, .signalSemaphoreCount = signal_bin ? 2 : 1, .pSignalSemaphores = sigs};
    g_commit_t[v % 64] = now_ns();
    pthread_mutex_lock(&g_qmu); VKC(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); pthread_mutex_unlock(&g_qmu);
    g_slot[g_cur_slot].fence = v;
    pthread_mutex_lock(&g_post_mu);
    if (g_post_cur) { g_post_cur->fence = v; if (g_post_tail) g_post_tail->next = g_post_cur; else g_post_head = g_post_cur; g_post_tail = g_post_cur; g_post_cur = NULL; }
    atomic_store(&g_fence, v);
    pthread_cond_broadcast(&g_post_cv);
    pthread_mutex_unlock(&g_post_mu);
    g_rec = 0; g_sub_ring = 0;
    pthread_mutex_unlock(&g_open_mu);
}
static void sub_commit(void) {
    pthread_mutex_lock(&g_mu);
    submit_locked(VK_NULL_HANDLE, VK_NULL_HANDLE);
    pthread_mutex_unlock(&g_mu);
}
// A sync's fence counts as reached DM_VK_FENCE_LAG submissions early (default 1). Everything the GPU reads from game memory was copied when the submission was replayed, so
// once it is submitted the game may reuse its buffers; what the wait really bounds is how far the CPU runs ahead. DMC fences and waits for each frame right after submitting it
// (one submission a frame), which on the Switch costs little; here it kept the CPU and GPU from ever overlapping, and the GPU's idle share kept Samsung's governor at its lowest
// clock (156 of 702 MHz). With a lag of 1 the CPU builds frame n + 1 while the GPU draws frame n. What arrives late: counter reports and event writes of the last submission
static uint64_t fence_lag(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_FENCE_LAG"); v = e ? atoi(e) : 1; if (v < 0) v = 0; if (v > NSLOT - 2) v = NSLOT - 2; } return (uint64_t)v; }
static int replayed_wait(uint64_t j, uint64_t timeout_ns);
static uint64_t job_value(uint64_t j);
// fence: a job number (mtl_last_fence). Reached once the job has been replayed (everything it reads from game memory is copied) and the GPU has completed the job
// fence_lag() jobs before it
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    uint64_t t = now_ns();
    if (!replayed_wait(fence, timeout_ns)) return 0;
    uint64_t lag = fence_lag();
    if (fence <= lag) return 1;
    uint64_t v = job_value(fence - lag);
    if (v <= atomic_load(&g_done)) return 1;
    if (!g_dev || !timeout_ns) return 0;
    uint64_t spent = now_ns() - t; int r = vk_wait(v, timeout_ns == UINT64_MAX ? UINT64_MAX : timeout_ns > spent ? timeout_ns - spent : 1); g_wait_ns[0] += now_ns() - t; g_wait_n[0]++;
    return r;
}
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { memmove((void *)dst, (const void *)src, n); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { pthread_mutex_lock(&g_mu); post_add((post_t){1, val, mode, v, 0, 0}); pthread_mutex_unlock(&g_mu); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { pthread_mutex_lock(&g_mu); post_add((post_t){2, NULL, 0, 0, addr, type}); pthread_mutex_unlock(&g_mu); }

// ---------- Memory pools: the game's memory; nothing to register (draw data is copied) ----------
void mtl_pool_add(const void *mem, uint64_t size) { (void)mem; (void)size; }
void mtl_pool_del(const void *mem) { (void)mem; }
void mtl_alias_drop_pool(const void *pool) { (void)pool; }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) { (void)len; *buf = NULL; *off = addr; return 1; }
static void set_depth_zero_now(int on) { if (g_depth_zero != on) VLOG("depth mode: near = %s", on ? "0" : "-w (GL)"); g_depth_zero = on; }
void mtl_set_origin_lower_left(int on) { if (on) RED1("window origin lower left (GL) not implemented: drawn as upper left"); }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }

// CPU read of a texture (ReadTexels): a GPU-written one is read back (the GPU work before it is finished first); others from their storage
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    size_t rows = (size_t)(r->height > 0 ? r->height : 1), layers = (size_t)(r->depth > 0 ? r->depth : 1);
    for (size_t z = 0; z < layers; z++) memset(dst + z * img, 0, row * rows);
    (void)t; (void)level; (void)layer0;
    RED1("ReadTexels returns zeros (readback not implemented)");
}

// ---------- Presentation ----------
static double g_present_t[64]; static uint64_t g_present_job[64], g_ticket;   // by ticket: when it was queued, its job (the fence after which the texture is free again); tickets issued
static VkSwapchainKHR g_sc; static VkSurfaceKHR g_surf; static VkImage g_sc_img[8]; static uint32_t g_sc_n, g_sc_w, g_sc_h; static VkFormat g_sc_fmt;
static VkSemaphore g_acq[8], g_rel[8]; static int g_sem_i;
static ANativeWindow *g_sc_win;
static void swapchain_make(void) {
    if (g_sc_win != dm_android_window) {
        if (g_sc) { vkDeviceWaitIdle(g_dev); vkDestroySwapchainKHR(g_dev, g_sc, NULL); g_sc = VK_NULL_HANDLE; }
        if (g_surf) { vkDestroySurfaceKHR(g_inst, g_surf, NULL); g_surf = VK_NULL_HANDLE; }
        g_sc_win = dm_android_window;
        if (!g_sc_win) return;
        VkAndroidSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = g_sc_win};
        VKC(vkCreateAndroidSurfaceKHR(g_inst, &si, NULL, &g_surf));
    }
    if (g_sc || !g_surf) return;
    VkSurfaceCapabilitiesKHR caps; VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surf, &caps));
    VkSurfaceFormatKHR sf[16]; uint32_t nf = 16; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, sf);
    g_sc_fmt = sf[0].format; for (uint32_t i = 0; i < nf; i++) if (sf[i].format == VK_FORMAT_R8G8B8A8_UNORM) g_sc_fmt = sf[i].format;
    // the window's own size and no pre-rotation: the compositor turns the landscape picture for the panel
    g_sc_w = (uint32_t)ANativeWindow_getWidth(g_sc_win); g_sc_h = (uint32_t)ANativeWindow_getHeight(g_sc_win);
    if (caps.currentExtent.width != 0xffffffffu && (caps.currentExtent.width != g_sc_w || caps.currentExtent.height != g_sc_h)) VLOG("surface extent %ux%u, window %ux%u, transform %#x",
        caps.currentExtent.width, caps.currentExtent.height, g_sc_w, g_sc_h, caps.currentTransform);
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surf, .minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount,
        .imageFormat = g_sc_fmt, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = {g_sc_w, g_sc_h}, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VKC(vkCreateSwapchainKHR(g_dev, &ci, NULL, &g_sc));
    g_sc_n = 8; VKC(vkGetSwapchainImagesKHR(g_dev, g_sc, &g_sc_n, g_sc_img));
    for (int i = 0; i < 8; i++) if (!g_acq[i]) { VkSemaphoreCreateInfo s = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(g_dev, &s, NULL, &g_acq[i])); VKC(vkCreateSemaphore(g_dev, &s, NULL, &g_rel[i])); }
    VLOG("swapchain %ux%u, %u images, format %d, transform %#x", g_sc_w, g_sc_h, g_sc_n, g_sc_fmt, caps.currentTransform);
}
// DM_VK_SHOT=<dir>[:every]: the presented frame as <dir>/frame_<n>.ppm (no window builds), every 60th by default
static void shot(vtex *x) {
    static int every = -1; static char dir[256];
    if (every < 0) { const char *e = getenv("DM_VK_SHOT"); every = 0; if (e) { snprintf(dir, sizeof dir, "%s", e); char *c = strchr(dir, ':'); every = 60; if (c) { *c = 0; every = atoi(c + 1); } } }
    if (!every || g_seq % (uint64_t)every || x->f.vk != VK_FORMAT_R8G8B8A8_UNORM) return;
    size_t n = (size_t)x->w * x->h * 4;
    VkDeviceMemory m; void *map; VkBuffer b = buf_new(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &m, &map);
    uint64_t ser = x->serial;
    sub_begin(); pthread_mutex_lock(&g_mu);
    if (!(x = tex_find(ser))) { submit_locked(VK_NULL_HANDLE, VK_NULL_HANDLE); pthread_mutex_unlock(&g_mu); vkDestroyBuffer(g_dev, b, NULL); vkFreeMemory(g_dev, m, NULL); return; }
    barrier_full(g_cmd);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {x->w, x->h, 1}};
    vkCmdCopyImageToBuffer(g_cmd, x->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    submit_locked(VK_NULL_HANDLE, VK_NULL_HANDLE); uint64_t v = atomic_load(&g_fence);
    pthread_mutex_unlock(&g_mu); vk_wait(v, UINT64_MAX);
    char p[300]; snprintf(p, sizeof p, "%s/frame_%05llu.ppm", dir, (unsigned long long)g_seq);
    FILE *f = fopen(p, "wb");
    if (f) { fprintf(f, "P6\n%u %u\n255\n", x->w, x->h); const uint8_t *s = map; for (size_t i = 0; i < (size_t)x->w * x->h; i++) fwrite(s + 4 * i, 1, 3, f); fclose(f); VLOG("frame %llu → %s", (unsigned long long)g_seq, p); }
    vkDestroyBuffer(g_dev, b, NULL); vkFreeMemory(g_dev, m, NULL);
}
// The present job (the replay thread): t is a copy of the window texture; g_seq is the present's ticket
static void present_now(const tex_t *t, const int crop[4]) {
    static uint64_t trace_n = 1;   // DM_VK_TRACE=<first>[:<count>]
    if (!g_trace_frame) { const char *e = getenv("DM_VK_TRACE"); g_trace_frame = e ? strtoull(e, NULL, 0) : ~0ull; const char *c = e ? strchr(e, ':') : NULL; if (c) trace_n = strtoull(c + 1, NULL, 0); }
    TR("present texture %llu", (unsigned long long)(t ? t->serial : 0));
    g_seq++;
    g_trace = g_seq >= g_trace_frame && g_seq - g_trace_frame < trace_n;
    if (g_trace && getenv("DM_VK_TRACE_DRAWS")) g_trace = 2;
    ts_report();
    if (!g_dev || !t) return;
    pthread_mutex_lock(&g_mu);
    vtex *x = tex_find(t->serial);
    if (!x) { pthread_mutex_unlock(&g_mu); return; }
    if (&dm_android_window == NULL) { pthread_mutex_unlock(&g_mu); shot(x); return; }   // the command-line build: no window
    swapchain_make();
    static uint32_t lw, lh, lsw, lsh;
    if (x->w != lw || x->h != lh || g_sc_w != lsw || g_sc_h != lsh) { lw = x->w; lh = x->h; lsw = g_sc_w; lsh = g_sc_h; LOG("[vk] present %ux%u, surface %ux%u", lw, lh, lsw, lsh); }
    if (!g_sc) { pthread_mutex_unlock(&g_mu); return; }
    int si = g_sem_i; g_sem_i = (g_sem_i + 1) % 8;
    uint32_t idx; VkResult ar = vkAcquireNextImageKHR(g_dev, g_sc, 1000000000ull, g_acq[si], VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(g_dev); vkDestroySwapchainKHR(g_dev, g_sc, NULL); g_sc = VK_NULL_HANDLE; pthread_mutex_unlock(&g_mu); return; }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { RED1("vkAcquireNextImageKHR: %d", ar); pthread_mutex_unlock(&g_mu); return; }
    pthread_mutex_unlock(&g_mu);
    // the present's own submission: blit the frame (cropped, scaled to fit with its aspect kept) into the swapchain image
    sub_begin(); pthread_mutex_lock(&g_mu);
    x = tex_find(t->serial);   // it may have been released while g_mu was not held
    if (!x || !g_sc) { submit_locked(g_acq[si], VK_NULL_HANDLE); pthread_mutex_unlock(&g_mu); return; }
    VkImage im = g_sc_img[idx]; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im, .subresourceRange = rr};
    barrier_full(g_cmd);
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue black = {{0, 0, 0, 1}}; vkCmdClearColorImage(g_cmd, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    int cx = 0, cy = 0, cw = (int)x->w, ch = (int)x->h;
    if (crop && crop[2] > 0 && crop[3] > 0) { cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3]; }
    // DM_SCALE=fill stretches the frame onto the whole surface (a 4:3 panel gets a 4:3 picture from the game's 16:9 one);
    // =crop scales it to cover and takes the centre (no bars, aspect kept, the sides trimmed); unset (or fit) letterboxes it
    const char *mode = getenv("DM_SCALE");
    int dw = (int)g_sc_w, dh = (int)g_sc_h, dx = 0, dy = 0;
    if (mode && !strcmp(mode, "crop")) {
        double s = (double)g_sc_w / cw > (double)g_sc_h / ch ? (double)g_sc_w / cw : (double)g_sc_h / ch;
        int sw = (int)((double)g_sc_w / s), sh = (int)((double)g_sc_h / s);
        cx += (cw - sw) / 2; cy += (ch - sh) / 2; cw = sw; ch = sh;
    } else if (!(mode && !strcmp(mode, "fill"))) {
        double s = (double)g_sc_w / cw < (double)g_sc_h / ch ? (double)g_sc_w / cw : (double)g_sc_h / ch;
        dw = (int)(cw * s); dh = (int)(ch * s); dx = ((int)g_sc_w - dw) / 2; dy = ((int)g_sc_h - dh) / 2;
    }
    static int mlog; if (!mlog) { mlog = 1; LOG("[vk] scale '%s': src %d,%d %dx%d -> dst %d,%d %dx%d on %ux%u", mode ? mode : "fit", cx, cy, cw, ch, dx, dy, dw, dh, g_sc_w, g_sc_h); }
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{cx, cy, 0}, {cx + cw, cy + ch, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    vkCmdBlitImage(g_cmd, x->img, VK_IMAGE_LAYOUT_GENERAL, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    submit_locked(g_acq[si], g_rel[si]);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_rel[si], .swapchainCount = 1, .pSwapchains = &g_sc, .pImageIndices = &idx};
    pthread_mutex_lock(&g_qmu); VkResult pr = vkQueuePresentKHR(g_q, &pi); pthread_mutex_unlock(&g_qmu);
    // SUBOPTIMAL is Android's note that the swapchain is not pre-rotated for the panel (the compositor rotates it): kept; only a changed surface makes a new one
    if (pr == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(g_dev); vkDestroySwapchainKHR(g_dev, g_sc, NULL); g_sc = VK_NULL_HANDLE; }
    pthread_mutex_unlock(&g_mu);
    return;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }

// ---------- The replay thread ----------
// The game's submit thread only queues: a submission's command handles (the game keeps the commands themselves until their fence, as the GPU reads them on the Switch),
// presents, and every change to what the replay reads (texture / sampler registrations, releases, CPU writes, the depth mode), in call order. One thread replays them in that
// order, so the replay sees exactly the state it saw when it ran on the submit thread, and the game's thread no longer waits for it (DMC's replay is 6 to 12 ms a frame
// of the 16.7). Submissions and presents are numbered jobs, which are the fence values the game sees: a job's fence is reached once the job is replayed (the copies of game
// memory are taken) and the GPU has finished the job fence_lag() before it (mtl_wait)
// Limit: objects the commands point to (programs, render-target textures, syncs) are read when the job is replayed, up to MAX_AHEAD jobs after the submit. A game that
// re-sets a program's shaders or re-initializes a texture right after submitting would have the earlier submission see the new state; DMC sets both once
enum { J_SUB, J_PRESENT, J_TEX_RELEASE, J_TEX_CPU, J_TEX_CPU_UNLESS_GPU, J_REG_TEX, J_REG_SAMP, J_PROG_RELEASE, J_DEPTH };
typedef struct {
    int kind, id, n, has_t, has_v, crop_on, crop[4]; uint64_t j, serial; uint64_t *h;
    tex_t t; view_t v; samp_par sp;
} job_t;
enum { JQ = 256, JMAP = 1024, MAX_AHEAD = 3 };   // queued operations; job → timeline value map; submissions the game may run ahead of the replay
static job_t g_jq[JQ]; static unsigned g_jq_head, g_jq_n;
static pthread_mutex_t g_jq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_jq_cv = PTHREAD_COND_INITIALIZER;   // the queue and replay progress
static uint64_t g_jobs, g_job_val[JMAP];   // jobs issued; timeline value at the end of each replayed job
static _Atomic uint64_t g_replayed;
static uint64_t *g_sub_h; static int g_sub_hn;   // the submission being queued (mtl_sub_index)
static void replay_start(void);
static void jq_push(const job_t *jb) {
    replay_start();
    pthread_mutex_lock(&g_jq_mu);
    while (g_jq_n == JQ) pthread_cond_wait(&g_jq_cv, &g_jq_mu);
    g_jq[(g_jq_head + g_jq_n++) % JQ] = *jb;
    pthread_cond_broadcast(&g_jq_cv);
    pthread_mutex_unlock(&g_jq_mu);
}
static void *replay_main(void *u) {
    (void)u;
    for (;;) {
        pthread_mutex_lock(&g_jq_mu);
        while (!g_jq_n) pthread_cond_wait(&g_jq_cv, &g_jq_mu);
        job_t jb = g_jq[g_jq_head]; g_jq_head = (g_jq_head + 1) % JQ; g_jq_n--;
        pthread_cond_broadcast(&g_jq_cv);
        pthread_mutex_unlock(&g_jq_mu);
        g_cur_job = jb.j;
        switch (jb.kind) {
            case J_SUB:
                sub_begin();
                for (int i = 0; i < jb.n; i++) { g_rest = jb.h + i + 1; g_nrest = jb.n - i - 1; exec_handle((const uint8_t *)jb.h[i]); }
                g_rest = NULL; g_nrest = 0;
                sub_commit(); free(jb.h); break;
            case J_PRESENT: present_now(jb.has_t ? &jb.t : NULL, jb.crop_on ? jb.crop : NULL); break;
            case J_TEX_RELEASE: tex_release_now(jb.serial); break;
            case J_TEX_CPU: case J_TEX_CPU_UNLESS_GPU: tex_cpu_written_now(jb.serial, jb.kind == J_TEX_CPU_UNLESS_GPU); break;
            case J_REG_TEX: reg_tex_now(jb.id, jb.has_t ? &jb.t : NULL, jb.has_v ? &jb.v : NULL); break;
            case J_REG_SAMP: reg_samp_now(jb.id, &jb.sp); break;
            case J_PROG_RELEASE: prog_release_now(jb.serial); break;
            case J_DEPTH: set_depth_zero_now(jb.id); break;
        }
        if (jb.j) {
            pthread_mutex_lock(&g_jq_mu);
            g_job_val[jb.j % JMAP] = atomic_load(&g_fence); atomic_store(&g_replayed, jb.j);
            pthread_cond_broadcast(&g_jq_cv);
            pthread_mutex_unlock(&g_jq_mu);
        }
    }
    return NULL;
}
static void replay_thread_new(void) {
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);
    pthread_t th; if (pthread_create(&th, &a, replay_main, NULL)) die("[vk] replay thread");
    pthread_setname_np(th, "dm-vk-replay"); pthread_attr_destroy(&a);
}
static void replay_start(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, replay_thread_new); }
// numbered job: issued under g_jq_mu so numbers follow queue order
static uint64_t jq_push_job(job_t *jb) {
    replay_start();
    pthread_mutex_lock(&g_jq_mu);
    while (g_jq_n == JQ || g_jobs - atomic_load(&g_replayed) >= MAX_AHEAD) pthread_cond_wait(&g_jq_cv, &g_jq_mu);
    jb->j = ++g_jobs;
    g_jq[(g_jq_head + g_jq_n++) % JQ] = *jb;
    pthread_cond_broadcast(&g_jq_cv);
    pthread_mutex_unlock(&g_jq_mu);
    return jb->j;
}
static int replayed_wait(uint64_t j, uint64_t timeout_ns) {
    if (atomic_load(&g_replayed) >= j) return 1;
    if (!timeout_ns) return 0;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t dl = timeout_ns >= (1ull << 62) ? UINT64_MAX : (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + timeout_ns;   // as vk_wait: a huge timeout is forever
    pthread_mutex_lock(&g_jq_mu);
    while (atomic_load(&g_replayed) < j) {
        if (dl == UINT64_MAX) { pthread_cond_wait(&g_jq_cv, &g_jq_mu); continue; }
        ts.tv_sec = (time_t)(dl / 1000000000ull); ts.tv_nsec = (long)(dl % 1000000000ull);
        if (pthread_cond_timedwait(&g_jq_cv, &g_jq_mu, &ts)) break;
    }
    int ok = atomic_load(&g_replayed) >= j;
    pthread_mutex_unlock(&g_jq_mu);
    return ok;
}
// the timeline value that completes job j (replayed); a job older than the map holds is long complete
static uint64_t job_value(uint64_t j) {
    pthread_mutex_lock(&g_jq_mu); uint64_t v = atomic_load(&g_replayed) - j < JMAP ? g_job_val[j % JMAP] : 0; pthread_mutex_unlock(&g_jq_mu);
    return v;
}

void mtl_sub_begin(void) { replay_start(); g_sub_hn = 0; }
void mtl_sub_index(const uint64_t *h, int n) {
    g_sub_h = malloc((size_t)(n > 0 ? n : 1) * sizeof *h); if (!g_sub_h) die("[vk] submission");
    if (n > 0) memcpy(g_sub_h, h, (size_t)n * sizeof *h);
    g_sub_hn = n;
}
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec(const uint8_t *handle) { (void)handle; }   // the handles came with mtl_sub_index
uint64_t mtl_sub_commit(void) {
    job_t jb = {.kind = J_SUB, .h = g_sub_h, .n = g_sub_hn};
    if (!jb.h) { jb.h = malloc(sizeof *jb.h); jb.n = 0; }
    g_sub_h = NULL; g_sub_hn = 0;
    return jq_push_job(&jb);
}
uint64_t mtl_last_fence(void) { pthread_mutex_lock(&g_jq_mu); uint64_t j = g_jobs; pthread_mutex_unlock(&g_jq_mu); return j; }
void mtl_finish(void) {
    replayed_wait(mtl_last_fence(), UINT64_MAX);   // also before the replay thread has made the device: the CPU-side copies of the queued jobs must be done
    pthread_mutex_lock(&g_mu); int dev = g_dev != VK_NULL_HANDLE; pthread_mutex_unlock(&g_mu);
    if (dev) vk_wait(atomic_load(&g_fence), UINT64_MAX);
}
void mtl_drain(void) { mtl_finish(); }
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    job_t jb = {.kind = J_PRESENT, .has_t = t != NULL, .crop_on = crop != NULL};
    if (t) jb.t = *t;
    if (crop) memcpy(jb.crop, crop, sizeof jb.crop);
    uint64_t j = jq_push_job(&jb);
    uint64_t tk = ++g_ticket; g_present_t[tk % 64] = now_ns() / 1e9; g_present_job[tk % 64] = j;
    return tk;
}
void mtl_tex_release(tex_t *t) { if (t->serial) jq_push(&(job_t){.kind = J_TEX_RELEASE, .serial = t->serial}); }
void mtl_tex_cpu_written(const tex_t *t) { if (t && t->serial) jq_push(&(job_t){.kind = J_TEX_CPU, .serial = t->serial}); }
void mtl_tex_cpu_written_unless_gpu(const tex_t *t) { if (t && t->serial) jq_push(&(job_t){.kind = J_TEX_CPU_UNLESS_GPU, .serial = t->serial}); }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    job_t jb = {.kind = J_REG_TEX, .id = id, .has_t = t != NULL, .has_v = v != NULL};
    if (t) jb.t = *t;
    if (v) jb.v = *v;
    jq_push(&jb);
}
void mtl_reg_samp(int id, const samp_par *p) { if (p) jq_push(&(job_t){.kind = J_REG_SAMP, .id = id, .sp = *p}); }
void mtl_prog_release(prog_t *p) { if (p->serial) jq_push(&(job_t){.kind = J_PROG_RELEASE, .serial = p->serial}); }
void mtl_set_depth_zero(int on) { jq_push(&(job_t){.kind = J_DEPTH, .id = on}); }
// activity.c: the window came (w) or is going (NULL; it may not be used once this returns)
void dm_vk_set_window(ANativeWindow *w) {
    pthread_mutex_lock(&g_mu);
    if (!w && g_dev) {
        if (g_sc) { vkDeviceWaitIdle(g_dev); vkDestroySwapchainKHR(g_dev, g_sc, NULL); g_sc = VK_NULL_HANDLE; }
        if (g_surf) { vkDestroySurfaceKHR(g_inst, g_surf, NULL); g_surf = VK_NULL_HANDLE; }
        g_sc_win = NULL;
    }
    dm_android_window = w;
    pthread_mutex_unlock(&g_mu);
}
// hle_nvn.c nvnWindowAcquireTexture: the texture's last present (ticket) read it in the submission it was queued after, or in the present's own blit
uint64_t mtl_acquire_fence(uint64_t ticket) { return ticket && g_ticket - ticket < 64 ? g_present_job[ticket % 64] : 0; }
double mtl_presented_time(uint64_t n) { return g_present_t[n % 64]; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_has_window(void) { return 0; }   // hle_nvn.c paces presents itself at 60 Hz; FIFO presentation keeps the swapchain in step
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_fps_target(void) { return 60; }
int mtl_display_hz(void) { return 60; }
static uint64_t g_fence_600;
void mtl_set_frame(uint64_t f) {
    (void)f;
    static uint64_t n, t0, rb, up;
    if (++n % 600 == 0) {
        uint64_t t = now_ns();
        VLOG("600 frames: waits per frame: syncs %.2f ms (%.1f), slots %.2f ms (%.1f), ring %.2f ms (%.1f); %.1f submissions per frame",
             atomic_exchange(&g_wait_ns[0], 0) / 600e6, atomic_exchange(&g_wait_n[0], 0) / 600.0, atomic_exchange(&g_wait_ns[1], 0) / 600e6, atomic_exchange(&g_wait_n[1], 0) / 600.0,
             atomic_exchange(&g_wait_ns[2], 0) / 600e6, atomic_exchange(&g_wait_n[2], 0) / 600.0, (atomic_load(&g_fence) - g_fence_600) / 600.0);
        g_fence_600 = atomic_load(&g_fence);
        VLOG("600 frames: %.1f fps, replay %.2f ms + GPU %.2f ms per frame, %llu passes / %llu draws per frame, ring %.2f MB / frame, %llu uploads (%.1f MB), %llu pipelines", t0 ? 600e9 / (t - t0) : 0,
             atomic_exchange(&g_replay_ns, 0) / 600e6, atomic_exchange(&g_gpu_ns, 0) / 600e6,
             (unsigned long long)(g_frame_passes / 600), (unsigned long long)(g_frame_draws / 600), (g_ring_bytes - rb) / 600.0 / 1048576, (unsigned long long)(g_uploads - up),
             g_upload_bytes / 1048576.0, (unsigned long long)g_pipes_made);
        t0 = t; rb = g_ring_bytes; up = g_uploads; g_frame_passes = g_frame_draws = 0; g_upload_bytes = 0;
    }
}
void mtl_ub_present(double iv_ms) { (void)iv_ms; }

// ---------- Statistics the host prints (zeros where this backend has no counterpart) ----------
void mtl_vbuf_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d) { *a = *b = *c = *d = 0; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *n) { *a = *b = 0; *n = 0; }
void mtl_args_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_bw(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_create_ns(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_create_stats(uint64_t *a, uint64_t *b) { *a = 0; *b = g_pipes_made; }
void mtl_pkg_stats(uint64_t *a, uint64_t *b) { *a = g_uploads; *b = 0; }
void mtl_stats(uint64_t *a, uint64_t *b) { *a = g_passes; *b = g_draws; }
void mtl_use_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return g_upload_bytes; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return -1; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void dm_gfx_shot_at(long k, const char *path) { (void)k; (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_passdump_next(long n) { (void)n; }
