// Texture patches: rectangles of RGBA pixels blended (source over) onto a game texture as it is uploaded, found by the hash of the texture's source bytes (nvn_vk.c tex_upload).
// nx2apk uses them for Xbox button prompts: the game's Switch button pictures are replaced on the phone, so nothing from the game leaves it.
//   DM_TEXPATCH=<dir>: <dir>/<hash>_L<level>_<w>x<h>.patch files, each "DMTP", u32 count, then per rectangle u32 x, y, w, h and w * h RGBA bytes (tools/prompts.py).
//     Only a texture of a size some patch names is hashed (tex_patch_wants), so ordinary uploads (a movie texture every frame) cost one table scan
//   DM_VK_TEXDUMP=<dir>: level 0 of every RGBA8 texture that is not linear (BC-decoded ones included) as <dir>/<hash>_L0_<w>x<h>.pam, once each (to find the textures to patch)
#include "../host/hle.h"
#include <dirent.h>

typedef struct { uint64_t hash; uint32_t lv, w, h; char *path; } patch_file;
enum { MAX_RECT = 4096 };   // a patch rectangle's largest side
static patch_file *g_pf; static int g_npf = -1;
static pthread_mutex_t g_tp_mu = PTHREAD_MUTEX_INITIALIZER;

static void tp_index(void) {   // the patch directory's file names, read once
    g_npf = 0;
    const char *dir = getenv("DM_TEXPATCH"); if (!dir) return;
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        unsigned long long h; unsigned lv, w, ht; char tail[16];
        if (sscanf(e->d_name, "%16llx_L%u_%ux%u.%15s", &h, &lv, &w, &ht, tail) != 5 || strcmp(tail, "patch")) continue;
        patch_file *n2 = realloc(g_pf, (size_t)(g_npf + 1) * sizeof *g_pf); if (!n2) break; g_pf = n2;
        size_t n = strlen(dir) + strlen(e->d_name) + 2; char *p = malloc(n); if (!p) break; snprintf(p, n, "%s/%s", dir, e->d_name);
        g_pf[g_npf++] = (patch_file){h, lv, w, ht, p};
    }
    closedir(d);
    LOG("[texpatch] %d patch files in %s", g_npf, dir);
}
static void tp_apply(const char *path, uint32_t w, uint32_t h, uint8_t *rgba, size_t pitch) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    char magic[4]; uint32_t n = 0, r[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "DMTP", 4) || fread(&n, 4, 1, f) != 1) { fclose(f); LOG("[texpatch] [RED] %s: not a patch file", path); return; }
    uint32_t applied = 0;
    for (uint32_t i = 0; i < n && fread(r, 4, 4, f) == 4; i++) {
        if (!r[2] || !r[3] || r[2] > MAX_RECT || r[3] > MAX_RECT) { LOG("[texpatch] [RED] %s: rectangle %u of %ux%u, not a patch", path, i, r[2], r[3]); break; }
        size_t row = (size_t)r[2] * 4; uint8_t *buf = malloc(row * r[3]);
        if (!buf || fread(buf, 1, row * r[3], f) != row * r[3]) { free(buf); break; }
        if (r[2] <= w && r[0] <= w - r[2] && r[3] <= h && r[1] <= h - r[3]) {   // compared without adding: no wrap-around
            for (uint32_t y = 0; y < r[3]; y++) for (uint32_t x = 0; x < r[2]; x++) {
                const uint8_t *s = buf + y * row + x * 4; uint8_t *d = rgba + (r[1] + y) * pitch + (size_t)(r[0] + x) * 4; unsigned a = s[3];
                for (int k = 0; k < 3; k++) d[k] = (uint8_t)((s[k] * a + d[k] * (255 - a) + 127) / 255);
                d[3] = (uint8_t)(a + (d[3] * (255 - a) + 127) / 255);
            }
            applied++;
        }
        else LOG("[texpatch] [RED] %s: rectangle %u outside the %ux%u texture", path, i, w, h);
        free(buf);
    }
    fclose(f);
    LOG("[texpatch] %s: %u of %u rectangles", path, applied, n);
}
static void tp_dump(uint64_t hash, uint32_t w, uint32_t h, const uint8_t *rgba, size_t pitch) {
    static uint64_t seen[4096]; static int nseen;
    const char *dir = getenv("DM_VK_TEXDUMP"); if (!dir) return;
    for (int i = 0; i < nseen; i++) if (seen[i] == hash) return;
    if (nseen < 4096) seen[nseen++] = hash;
    char p[512]; snprintf(p, sizeof p, "%s/%016llx_L0_%ux%u.pam", dir, (unsigned long long)hash, w, h);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
    for (uint32_t y = 0; y < h; y++) fwrite(rgba + y * pitch, 1, (size_t)w * 4, f);
    fclose(f);
}
// whether a texture level of this size needs hashing at all (a patch names the size, or dumping is on)
int tex_patch_wants(uint32_t lv, uint32_t w, uint32_t h) {
    pthread_mutex_lock(&g_tp_mu);
    if (g_npf < 0) tp_index();
    static int dump = -1; if (dump < 0) dump = getenv("DM_VK_TEXDUMP") != NULL;
    int r = dump && lv == 0;
    for (int i = 0; i < g_npf && !r; i++) r = g_pf[i].lv == lv && g_pf[i].w == w && g_pf[i].h == h;
    pthread_mutex_unlock(&g_tp_mu);
    return r;
}
void tex_patch(uint64_t hash, uint32_t lv, uint32_t w, uint32_t h, uint8_t *rgba, size_t pitch) {
    pthread_mutex_lock(&g_tp_mu);
    if (g_npf < 0) tp_index();
    if (lv == 0) tp_dump(hash, w, h, rgba, pitch);
    for (int i = 0; i < g_npf; i++) if (g_pf[i].hash == hash && g_pf[i].lv == lv && g_pf[i].w == w && g_pf[i].h == h) tp_apply(g_pf[i].path, w, h, rgba, pitch);
    pthread_mutex_unlock(&g_tp_mu);
}
