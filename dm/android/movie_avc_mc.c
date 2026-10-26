// H.264 (AVC) decoding for the movie decoder (host/hle_movie_avc.h) on Android's MediaCodec (the phone's hardware decoder), in place of hle_movie_avc.c's VideoToolbox.
// The SDK's demuxer hands over one access unit per packet with Annex B start codes and SPS / PPS in Configure's csd-0 / csd-1, which is MediaCodec's own input convention.
// Synchronous use: mavc_decode queues an access unit and collects whatever pictures are ready; pictures come out in presentation order, copied as NV12 into a small queue
// (the decoder's output buffer goes back at once); mavc_take converts the oldest into the caller's NV12 or RGBA frame. Output is a byte buffer, not a surface: the codec is asked
// for YUV420 semi-planar, and planar output (I420) is interleaved on the copy.
#include "../host/hle.h"
#include "../host/hle_movie_avc.h"
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define QMAX 8
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty, red, rgba;
    AMediaCodec *mc; int w, h, stride, slice, cfmt, crop[4];
    struct { uint8_t *nv12; int64_t pts; } q[QMAX]; int nq, pw, ph;   // decoded pictures (pw x ph NV12, tightly packed)
    uint8_t *au; size_t aucap;
    int eos_sent;
};

mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); return a; }
static void close_codec(mavc *a) { if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); a->mc = NULL; } a->eos_sent = 0; }
void mavc_drop(mavc *a) { for (int i = 0; i < a->nq; i++) free(a->q[i].nv12); a->nq = 0; if (a->mc) { AMediaCodec_flush(a->mc); a->eos_sent = 0; } }
void mavc_free(mavc *a) { if (!a) return; close_codec(a); for (int i = 0; i < a->nq; i++) free(a->q[i].nv12); free(a->au); free(a); }
int mavc_queued(const mavc *a) { return a->nq; }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
}
// NAL units of p (Annex B start codes, or else 4-byte lengths), each without its prefix (as hle_movie_avc.c)
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {   // AVCDecoderConfigurationRecord (the MP4 avcC box)
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}

static int open_codec(mavc *a) {
    close_codec(a);
    if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return 0; }
    a->mc = AMediaCodec_createDecoderByType("video/avc");
    if (!a->mc) { if (!a->red++) LOG("[RED] movie: no MediaCodec H.264 decoder, outputting black frames"); return 0; }
    uint8_t csd0[520] = {0, 0, 0, 1}, csd1[520] = {0, 0, 0, 1};
    memcpy(csd0 + 4, a->sps, (size_t)a->nsps); memcpy(csd1 + 4, a->pps, (size_t)a->npps);
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 1920); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 1088);   // the SPS gives the real size
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);   // COLOR_FormatYUV420SemiPlanar (NV12)
    AMediaFormat_setBuffer(f, "csd-0", csd0, (size_t)a->nsps + 4); AMediaFormat_setBuffer(f, "csd-1", csd1, (size_t)a->npps + 4);
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK || AMediaCodec_start(a->mc) != AMEDIA_OK) { if (!a->red++) LOG("[RED] movie: MediaCodec H.264 configure / start failed (%d)", st); AMediaCodec_delete(a->mc); a->mc = NULL; return 0; }
    LOG("[movie] AVC: MediaCodec decoding, profile %d", a->nsps > 1 ? a->sps[1] : 0);
    a->dirty = 0; a->cfmt = 21; a->w = a->h = 0;
    return 1;
}
static void out_format(mavc *a) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc);
    int32_t v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &v)) a->w = v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &v)) a->h = v;
    a->stride = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v ? v : a->w;
    a->slice = AMediaFormat_getInt32(f, "slice-height", &v) && v ? v : a->h;
    a->cfmt = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v) ? v : 21;
    int32_t l = 0, t = 0, r = a->w - 1, b = a->h - 1;
    if (AMediaFormat_getRect(f, "crop", &l, &t, &r, &b)) {}
    a->crop[0] = l; a->crop[1] = t; a->crop[2] = r - l + 1; a->crop[3] = b - t + 1;
    if (a->crop[2] <= 0 || a->crop[3] <= 0) { a->crop[0] = a->crop[1] = 0; a->crop[2] = a->w; a->crop[3] = a->h; }
    LOG("[movie] AVC output %dx%d stride %d slice %d colour format %#x crop %d,%d %dx%d", a->w, a->h, a->stride, a->slice, a->cfmt, a->crop[0], a->crop[1], a->crop[2], a->crop[3]);
    if (a->cfmt != 21 && a->cfmt != 19 && !a->red++) LOG("[RED] movie: decoder colour format %#x is neither NV12 (21) nor I420 (19): read as NV12", a->cfmt);
    AMediaFormat_delete(f);
}
// Collect the pictures the decoder has ready (wait up to timeout_us for the first); returns 1 at the end of the stream
static int drain(mavc *a, int64_t timeout_us) {
    for (;;) {
        AMediaCodecBufferInfo bi;
        ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, timeout_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { out_format(a); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return 0;
        timeout_us = 0;
        if (!a->w) out_format(a);
        size_t sz; uint8_t *buf = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &sz);
        int end = (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        if (buf && bi.size > 0) {
            if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC picture queue full, dropping the oldest"); free(a->q[0].nv12); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
            int cw = a->crop[2], ch = a->crop[3], cx = a->crop[0], cy = a->crop[1];
            uint8_t *d = malloc((size_t)cw * ch * 3 / 2 + cw); if (!d) die("movie: picture allocation failed");
            const uint8_t *y = buf + bi.offset, *c = y + (size_t)a->stride * a->slice;
            for (int r = 0; r < ch; r++) memcpy(d + (size_t)r * cw, y + (size_t)(r + cy) * a->stride + cx, (size_t)cw);
            uint8_t *duv = d + (size_t)cw * ch;
            if (a->cfmt == 19) {   // I420: U plane then V plane, half stride
                const uint8_t *u = c, *v = c + (size_t)(a->stride / 2) * (a->slice / 2);
                for (int r = 0; r < ch / 2; r++) for (int x = 0; x < cw / 2; x++) {
                    duv[(size_t)r * cw + 2 * x] = u[(size_t)(r + cy / 2) * (a->stride / 2) + x + cx / 2]; duv[(size_t)r * cw + 2 * x + 1] = v[(size_t)(r + cy / 2) * (a->stride / 2) + x + cx / 2]; }
            } else for (int r = 0; r < ch / 2; r++) memcpy(duv + (size_t)r * cw, c + (size_t)(r + cy / 2) * a->stride + (cx & ~1), (size_t)cw);
            int k = a->nq; while (k > 0 && a->q[k - 1].pts > bi.presentationTimeUs) { a->q[k] = a->q[k - 1]; k--; }
            a->q[k].nv12 = d; a->q[k].pts = bi.presentationTimeUs; a->nq++; a->pw = cw; a->ph = ch;
        }
        AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, false);
        if (end) return 1;
    }
}
typedef struct { mavc *a; size_t len; } au_ctx;
static void au_add(void *ctx, const uint8_t *nal, size_t n) {   // the access unit as Annex B, parameter sets kept aside
    au_ctx *c = ctx; mavc *a = c->a; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(a, nal, n); return; }
    if (t == 9) return;
    if (c->len + 4 + n > a->aucap) { a->aucap = (c->len + 4 + n) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + c->len; o[0] = 0; o[1] = 0; o[2] = 0; o[3] = 1; memcpy(o + 4, nal, n); c->len += 4 + n;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0}; each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if ((a->dirty || !a->mc) && !open_codec(a)) return -1;
    for (int tries = 0;; tries++) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 2000);
        if (i >= 0) {
            size_t cap; uint8_t *buf = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
            if (!buf || cap < c.len) { if (!a->red++) LOG("[RED] movie: AVC input buffer of %zu bytes for a %zu-byte access unit", cap, c.len); AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, (uint64_t)pts_us, 0); return -1; }
            memcpy(buf, a->au, c.len);
            AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, c.len, (uint64_t)pts_us, 0);
            break;
        }
        drain(a, 0);   // no input buffer free: the decoder waits for its output to be taken
        if (tries > 500) { if (!a->red++) LOG("[RED] movie: AVC decoder takes no input"); return -1; }
    }
    drain(a, 0);
    return 0;
}
// YUV (BT.601, video range, what the movies use) → RGBA
static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (a->mc) {
        if (flush && !a->eos_sent) {
            ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 10000);
            if (i >= 0) { AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); a->eos_sent = 1; }
        }
        if (flush && a->eos_sent) { for (int k = 0; k < 50 && !drain(a, 10000); k++) if (a->nq) break; }
        else drain(a, 0);
    }
    if (!a->nq) return 0;
    uint8_t *s = a->q[0].nv12; if (pts_us) *pts_us = a->q[0].pts;
    memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    int pw = a->pw, ph = a->ph, cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    const uint8_t *suv = s + (size_t)pw * ph;
    if (a->rgba) {
        if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
        for (int r = 0; r < chh; r++) for (int x = 0; x < cw; x++) {
            int Y = (s[(size_t)r * pw + x] - 16) * 298, U = suv[(size_t)(r / 2) * pw + (x & ~1)] - 128, V = suv[(size_t)(r / 2) * pw + (x & ~1) + 1] - 128;
            uint8_t *o = dst + ((size_t)r * w + x) * 4;
            o[0] = clamp8((Y + 409 * V + 128) >> 8); o[1] = clamp8((Y - 100 * U - 208 * V + 128) >> 8); o[2] = clamp8((Y + 516 * U + 128) >> 8); o[3] = 255;
        }
    } else {
        if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, s + (size_t)r * pw, (size_t)cw);
        for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, suv + (size_t)r * pw, (size_t)(cw & ~1));
    }
    free(s);
    return 1;
}
