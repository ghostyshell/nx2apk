// AudioConverter (shim/AudioToolbox/AudioToolbox.h) for the movie decoder's AAC (hle_movie.c md_aac) on Android's MediaCodec, in place of Apple's AudioToolbox.
// Only the use hle_movie.c makes: AAC → interleaved PCM16 at the same rate and channel count. The configuration comes as the MPEG-4 ES descriptor magic cookie hle_movie.c builds;
// its decoder-specific descriptor (tag 5) is the AudioSpecificConfig, MediaCodec's csd-0. Without one an AAC-LC configuration is made from the stream description.
// FillComplexBuffer takes packets from the input callback until it reports none, queues each, and collects the PCM that is ready. MediaCodec keeps a frame or two in flight,
// so output lags input: it waits for a picture only while more than LAG packets are outstanding, and PCM beyond what the caller asked for is kept for the next call.
#include "../host/hle.h"
#include <AudioToolbox/AudioToolbox.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define LAG 2
struct dm_aconv {
    AMediaCodec *mc; int rate, ch, och, red;   // och: the codec's output channel count (format change), converted to ch
    uint8_t asc[64]; int an;
    int64_t nin, nout, pts;
    int16_t *pend; size_t npend, cap;          // decoded frames not handed out yet (ch channels)
};

OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c) {
    if (in->mFormatID != kAudioFormatMPEG4AAC || out->mFormatID != kAudioFormatLinearPCM || out->mBitsPerChannel != 16 || in->mChannelsPerFrame < 1 || in->mChannelsPerFrame > 8) { *c = NULL; return -50; }
    struct dm_aconv *a = calloc(1, sizeof *a); if (!a) die("movie: AAC converter allocation failed");
    a->rate = (int)in->mSampleRate; a->ch = a->och = (int)in->mChannelsPerFrame;
    *c = a; return noErr;
}
// The ES descriptor's tag 5 payload (sizes in 7-bit groups, as esds_descr writes them)
OSStatus AudioConverterSetProperty(AudioConverterRef a, UInt32 id, UInt32 size, const void *data) {
    if (!a || id != kAudioConverterDecompressionMagicCookie) return -50;
    const uint8_t *p = data, *e = p + size;
    while (p < e) {
        int tag = *p++; unsigned n = 0; for (int i = 0; i < 4 && p < e; i++) { uint8_t b = *p++; n = n << 7 | (b & 0x7f); if (!(b & 0x80)) break; }
        if (tag == 5) { if (n > sizeof a->asc || n > (size_t)(e - p)) return -50; memcpy(a->asc, p, n); a->an = (int)n; return noErr; }
        if (tag == 3) p += 3; else if (tag == 4) p += 13; else p += n;   // descend into the ES and decoder configuration descriptors, skip any other
    }
    return -50;
}
static int open_codec(struct dm_aconv *a) {
    if (!a->an) {   // AAC-LC from the rate and channel count
        static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
        int fi = 3; for (int i = 0; i < 13; i++) if (rates[i] == a->rate) fi = i;
        a->asc[0] = (uint8_t)(2 << 3 | fi >> 1); a->asc[1] = (uint8_t)((fi & 1) << 7 | (a->ch == 8 ? 7 : a->ch) << 3); a->an = 2;
    }
    a->mc = AMediaCodec_createDecoderByType("audio/mp4a-latm");
    if (!a->mc) return 0;
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, a->rate);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, a->ch);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_IS_ADTS, 0);
    AMediaFormat_setInt32(f, "max-output-channel-count", a->ch);   // the AAC decoder otherwise downmixes to stereo (the documented key, and the spelling older decoders read)
    AMediaFormat_setInt32(f, "max-output-channel_count", a->ch);
    AMediaFormat_setBuffer(f, "csd-0", a->asc, (size_t)a->an);
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK || AMediaCodec_start(a->mc) != AMEDIA_OK) { AMediaCodec_delete(a->mc); a->mc = NULL; return 0; }
    LOG("[movie] AAC: MediaCodec decoding, %d channels %d Hz, AudioSpecificConfig %d bytes", a->ch, a->rate, a->an);
    return 1;
}
// one output buffer (if one comes within timeout µs) → pend, its och channels mapped to ch (missing ones silent); 1 if something was taken
static int drain_one(struct dm_aconv *a, int64_t timeout) {
    AMediaCodecBufferInfo bi;
    ssize_t o = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, timeout);
    if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc); int32_t n = 0;
        if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &n) && n > 0 && n <= 8) a->och = n;
        AMediaFormat_delete(f);
        if (a->och != a->ch && !a->red++) LOG("[RED] movie: the AAC decoder outputs %d channels for a %d-channel stream, remapped", a->och, a->ch);
        return 1;
    }
    if (o < 0) return 0;
    size_t sz; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)o, &sz);
    if (b && bi.size > 0 && (size_t)bi.offset + (size_t)bi.size <= sz) {
        size_t fr = (size_t)bi.size / (2u * (unsigned)a->och);
        if (a->npend + fr > a->cap) { a->cap = (a->npend + fr) * 2; a->pend = realloc(a->pend, a->cap * 2u * (unsigned)a->ch); if (!a->pend) die("movie: AAC buffer allocation failed"); }
        const int16_t *s = (const int16_t *)(b + bi.offset); int16_t *d = a->pend + a->npend * (size_t)a->ch;
        if (a->och == a->ch) memcpy(d, s, fr * 2u * (unsigned)a->ch);
        else for (size_t i = 0; i < fr; i++) for (int k = 0; k < a->ch; k++) d[i * (size_t)a->ch + (size_t)k] = k < a->och ? s[i * (size_t)a->och + (size_t)k] : 0;
        a->npend += fr;
    }
    AMediaCodec_releaseOutputBuffer(a->mc, (size_t)o, false);
    a->nout++;
    return 1;
}
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef a, AudioConverterComplexInputDataProc proc, void *user, UInt32 *frames, AudioBufferList *out, AudioStreamPacketDescription *desc) {
    (void)desc;
    UInt32 want = *frames; *frames = 0;
    if (!a || (!a->mc && !open_codec(a))) return -50;
    for (;;) {
        UInt32 np = 1; AudioBufferList bl = {1, {{0, 0, NULL}}}; AudioStreamPacketDescription *pd = NULL;
        if (proc(a, &np, &bl, &pd, user) || !np || !bl.mBuffers[0].mData) break;
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 20000);
        while (i < 0 && drain_one(a, 5000)) i = AMediaCodec_dequeueInputBuffer(a->mc, 20000);   // full: make room by taking output
        if (i < 0) { if (!a->red++) LOG("[RED] movie: the AAC decoder takes no input, packet dropped"); continue; }
        size_t cap; uint8_t *ib = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
        size_t n = bl.mBuffers[0].mDataByteSize < cap ? bl.mBuffers[0].mDataByteSize : cap;
        memcpy(ib, bl.mBuffers[0].mData, n);
        AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, n, (uint64_t)a->pts, 0);
        a->pts += 1024 * 1000000ll / (a->rate > 0 ? a->rate : 48000); a->nin++;
    }
    while (drain_one(a, a->nin - a->nout > LAG ? 20000 : 0)) {}
    if (a->nin - a->nout > LAG) a->nout = a->nin - LAG;   // nothing came within the wait: the decoder dropped or merged a packet, so stop expecting it (else every call would wait 20 ms)
    size_t give = a->npend < want ? a->npend : want;
    UInt32 room = out->mBuffers[0].mDataByteSize / (2u * (unsigned)a->ch); if (give > room) give = room;
    memcpy(out->mBuffers[0].mData, a->pend, give * 2u * (unsigned)a->ch);
    memmove(a->pend, a->pend + give * (size_t)a->ch, (a->npend - give) * 2u * (unsigned)a->ch); a->npend -= give;
    *frames = (UInt32)give;
    return noErr;
}
OSStatus AudioConverterDispose(AudioConverterRef a) {
    if (!a) return noErr;
    if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); }
    free(a->pend); free(a);
    return noErr;
}
