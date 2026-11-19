// SPDX-License-Identifier: GPL-3.0-or-later
// Opus -> PCM -> AAC-LC (see aacpipe.h).
//
// Threads: decode and encode may run on different threads; a mutex guards the PCM buffer and is never held
// while faac runs. faac 1.30 is double precision, so the binary's FP ABI must match the SoC (T41: FP64/NAN2008 via
// SOC=xburst2 builds); an FP32 build broke on T41 worker threads (homekit.md "faac off the main thread").
//
// faac versions: the bundled faac 1.30 (legacy faacEnc* API) or faac >= 2 (faac_encoder_* API, as Thingino's faac
// package ships it), picked by FAAC_VERSION_MAJOR, which only faac >= 2's header defines. faac 3.0 (ABI 3) renamed
// the faac_params fields and made bit_rate whole-stream; for mono the value is the same.
//
// Timing: the first PCM sample takes the packet's capture time; after that samples are counted, so AAC frames are
// exactly 1024 samples apart. faac's priming delay (1024 samples in both versions: output k starts at input sample
// k * 1024 - delay) is subtracted, and frames that start before sample 0 are dropped. A gap
// or jump of more than 300 ms in the capture times restarts the count, so audio stays aligned with video after
// a dropout.
#include "aacpipe.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "faac.h"
#include "opus.h"

#if defined(FAAC_VERSION_MAJOR) && FAAC_VERSION_MAJOR >= 2
#define FAAC_V2 1
#else
#define FAAC_V2 0
#endif
#if defined(FAAC_VERSION_MAJOR) && FAAC_VERSION_MAJOR >= 3
#define AACPIPE_OBJ_LC FAAC_OBJ_LC
#else
#define AACPIPE_OBJ_LC FAAC_OBJ_LOW
#endif

#define MAX_OPUS_SAMPLES 5760 // 120 ms at 48 kHz: the largest Opus packet, at any decode rate
#define RESYNC_US        300000
#define PCM_MAX          32000 // 2 s at 16 kHz waiting for the encoder

struct aacpipe {
    OpusDecoder* dec;
#if FAAC_V2
    faac_encoder* enc;
    const uint8_t* asc; // owned by the encoder
#else
    faacEncHandle enc;
    uint8_t* asc;
#endif
    uint32_t rate;
    uint32_t delay;     // encoder priming delay, samples
    unsigned long maxOut;
    size_t ascLen;
    pthread_mutex_t lock;
    int16_t pcm[PCM_MAX];
    size_t fill;          // samples waiting in pcm[]
    int64_t baseUs;       // capture time of sample 0 of the current run
    uint64_t inSamples;   // samples fed to faac in this run
    uint64_t outFrames;   // AAC frames received in this run (including the priming ones)
    bool started;
    uint8_t* out;
};

// AAC-LC, MPEG-4, mono, raw frames (the MP4 carries the config in esds), 16-bit input. TNS on with faac 3.x
// (it gates TNS per frame, so it costs nothing measurable when it doesn't help), off with 2.x.
static bool OpenEncoder(aacpipe_t* p, uint32_t sampleRate, uint32_t bitrate) {
#if FAAC_V2
    faac_params prm;
    if (faac_params_init(&prm, sizeof prm) != FAAC_OK)
        return false;
    prm.sample_rate = sampleRate;
    prm.bandwidth = 0;
    prm.input_format = FAAC_INPUT_16BIT;
#if FAAC_VERSION_MAJOR >= 3
    prm.channels = 1;
    prm.object_type = FAAC_OBJ_LC;
    prm.stereo_mode = FAAC_STEREO_LR;
    prm.flags = FAAC_FLAG_TNS | FAAC_FLAG_PNS;
    prm.bit_rate = bitrate;    // whole-stream; mono
    prm.stream_format = FAAC_STREAM_RAW;
#else
    prm.num_channels = 1;
    prm.mpeg_version = FAAC_MPEG4;
    prm.object_type = FAAC_OBJ_LOW;
    prm.joint_mode = FAAC_JOINT_NONE;
    prm.use_tns = false;
    prm.bit_rate = bitrate; // per channel; mono
    prm.output_format = FAAC_STREAM_RAW;
#endif
    if (faac_encoder_open(&prm, &p->enc) != FAAC_OK)
        return false;
    faac_encoder_info info = { .struct_size = sizeof info };
    uint32_t ascLen = 0;
    if (faac_encoder_get_info(p->enc, &info) != FAAC_OK || info.frame_samples != AACPIPE_FRAME_SAMPLES ||
        info.object_type != AACPIPE_OBJ_LC || faac_encoder_asc(p->enc, &p->asc, &ascLen) != FAAC_OK)
        return false;
    p->maxOut = info.max_output_bytes;
    p->delay = info.encoder_delay;
    p->ascLen = ascLen;
    return true;
#else
    unsigned long inputSamples = 0, ascLen = 0;
    p->enc = faacEncOpen(sampleRate, 1, &inputSamples, &p->maxOut);
    if (!p->enc || inputSamples != AACPIPE_FRAME_SAMPLES)
        return false;
    faacEncConfigurationPtr cfg = faacEncGetCurrentConfiguration(p->enc);
    cfg->aacObjectType = LOW;
    cfg->mpegVersion = MPEG4;
    cfg->useTns = 0;
    cfg->allowMidside = 0;
    cfg->bitRate = bitrate;
    cfg->bandWidth = 0;
    cfg->outputFormat = 0;
    cfg->inputFormat = FAAC_INPUT_16BIT;
    if (!faacEncSetConfiguration(p->enc, cfg) || faacEncGetDecoderSpecificInfo(p->enc, &p->asc, &ascLen))
        return false;
    p->ascLen = ascLen;
    p->delay = AACPIPE_FRAME_SAMPLES; // one priming frame
    return true;
#endif
}

aacpipe_t* aacpipe_open(uint32_t sampleRate, uint32_t bitrate) {
    aacpipe_t* p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    int err = 0;
    p->rate = sampleRate;
    pthread_mutex_init(&p->lock, NULL);
    p->dec = opus_decoder_create((opus_int32) sampleRate, 1, &err);
    if (!p->dec || err != OPUS_OK || !OpenEncoder(p, sampleRate, bitrate ? bitrate : 32000) ||
        !(p->out = malloc(p->maxOut))) {
        aacpipe_close(p);
        return NULL;
    }
    return p;
}

void aacpipe_close(aacpipe_t* p) {
    if (!p)
        return;
    if (p->dec)
        opus_decoder_destroy(p->dec);
#if FAAC_V2
    faac_encoder_close(&p->enc);
#else
    if (p->enc)
        faacEncClose(p->enc);
    free(p->asc);
#endif
    free(p->out);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

const uint8_t* aacpipe_asc(aacpipe_t* p, size_t* len) {
    *len = p->ascLen;
    return p->asc;
}

// Encodes complete blocks. The PCM lock is held only to take one block out, never while faac runs, so the
// decoding thread is never blocked behind the (slow, double precision) encoder.
static int Encode(aacpipe_t* p, aacpipe_out_fn out, void* ctx) {
    int emitted = 0;
    int16_t block[AACPIPE_FRAME_SAMPLES];
    for (;;) {
        pthread_mutex_lock(&p->lock);
        if (p->fill < AACPIPE_FRAME_SAMPLES) {
            pthread_mutex_unlock(&p->lock);
            break;
        }
        memcpy(block, p->pcm, sizeof block);
        memmove(p->pcm, p->pcm + AACPIPE_FRAME_SAMPLES, (p->fill - AACPIPE_FRAME_SAMPLES) * sizeof p->pcm[0]);
        p->fill -= AACPIPE_FRAME_SAMPLES;
        p->inSamples += AACPIPE_FRAME_SAMPLES;
        int64_t baseUs = p->baseUs;
        pthread_mutex_unlock(&p->lock);

#if FAAC_V2
        uint32_t written = 0;
        faac_status st = faac_encoder_encode(p->enc, block, AACPIPE_FRAME_SAMPLES, p->out, (uint32_t) p->maxOut,
                                             &written);
        int n = st == FAAC_OK ? (int) written : -1;
#else
        int n = faacEncEncode(p->enc, (int32_t*) block, AACPIPE_FRAME_SAMPLES, p->out, (unsigned int) p->maxOut);
#endif
        if (n <= 0)
            continue; // still filling the encoder's lookahead
        // Output k holds input samples starting at k * 1024 - delay; drop those that start before sample 0.
        uint64_t k = p->outFrames++;
        if (k * AACPIPE_FRAME_SAMPLES < p->delay)
            continue;
        int64_t ts = baseUs + (int64_t) ((k * AACPIPE_FRAME_SAMPLES - p->delay) * 1000000ULL / p->rate);
        out(ctx, p->out, (size_t) n, ts);
        emitted++;
    }
    return emitted;
}

int aacpipe_decode(aacpipe_t* p, const uint8_t* opus, size_t len, int64_t tsUs) {
    opus_int16 buf[MAX_OPUS_SAMPLES];
    int n = opus_decode(p->dec, opus, (opus_int32) len, buf, MAX_OPUS_SAMPLES, 0);
    if (n < 0)
        return -1;
    pthread_mutex_lock(&p->lock);
    int64_t expected = p->baseUs + (int64_t) ((p->inSamples + p->fill) * 1000000ULL / p->rate);
    if (!p->started || tsUs - expected > RESYNC_US || expected - tsUs > RESYNC_US) {
        // Start (or restart after a gap): new time base, fresh encoder state for the timestamps.
        p->started = true;
        p->baseUs = tsUs;
        p->inSamples = p->fill = 0;
        p->outFrames = 0;
    }
    if ((size_t) n > PCM_MAX - p->fill) { // the encoder fell 2 s behind: start a new run
        p->started = false;
        p->fill = 0;
        n = n > PCM_MAX ? PCM_MAX : n;
        p->baseUs = tsUs;
        p->inSamples = p->outFrames = 0;
        p->started = true;
    }
    memcpy(p->pcm + p->fill, buf, (size_t) n * sizeof buf[0]);
    p->fill += (size_t) n;
    int pending = (int) p->fill;
    pthread_mutex_unlock(&p->lock);
    return pending;
}

int aacpipe_encode(aacpipe_t* p, aacpipe_out_fn out, void* ctx) {
    return Encode(p, out, ctx); // locks per block itself
}
