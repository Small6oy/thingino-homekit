// SPDX-License-Identifier: GPL-3.0-or-later
// Opus -> PCM -> AAC-LC (see aacpipe.h).
//
// Threads: decode and encode may run on different threads; a mutex guards the PCM buffer and is never held
// while faac runs. faac is double precision, so the binary's FP ABI must match the SoC (T41: FP64/NAN2008 via
// SOC=xburst2 builds); an FP32 build broke on T41 worker threads (homekit.md "faac off the main thread").
//
// Timing: the first PCM sample takes the packet's capture time; after that samples are counted, so AAC frames are
// exactly 1024 samples apart. faac outputs with one frame of delay (its priming frame), which is subtracted. A gap
// or jump of more than 300 ms in the capture times restarts the count, so audio stays aligned with video after
// a dropout.
#include "aacpipe.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "faac.h"
#include "opus.h"

#define MAX_OPUS_SAMPLES 5760 // 120 ms at 48 kHz: the largest Opus packet, at any decode rate
#define RESYNC_US        300000
#define PCM_MAX          32000 // 2 s at 16 kHz waiting for the encoder

struct aacpipe {
    OpusDecoder* dec;
    faacEncHandle enc;
    uint32_t rate;
    unsigned long inputSamples, maxOut;
    uint8_t* asc;
    unsigned long ascLen;
    pthread_mutex_t lock;
    int16_t pcm[PCM_MAX];
    size_t fill;          // samples waiting in pcm[]
    int64_t baseUs;       // capture time of sample 0 of the current run
    uint64_t inSamples;   // samples fed to faac in this run
    uint64_t outFrames;   // AAC frames received in this run (including the priming frame)
    bool started;
    uint8_t* out;
};

aacpipe_t* aacpipe_open(uint32_t sampleRate, uint32_t bitrate) {
    aacpipe_t* p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    int err = 0;
    p->rate = sampleRate;
    pthread_mutex_init(&p->lock, NULL);
    p->dec = opus_decoder_create((opus_int32) sampleRate, 1, &err);
    p->enc = faacEncOpen(sampleRate, 1, &p->inputSamples, &p->maxOut);
    if (!p->dec || err != OPUS_OK || !p->enc || p->inputSamples != AACPIPE_FRAME_SAMPLES) {
        aacpipe_close(p);
        return NULL;
    }
    faacEncConfigurationPtr cfg = faacEncGetCurrentConfiguration(p->enc);
    cfg->aacObjectType = LOW;
    cfg->mpegVersion = MPEG4;
    cfg->useTns = 0;
    cfg->allowMidside = 0;
    cfg->bitRate = bitrate ? bitrate : 32000;
    cfg->bandWidth = 0;
    cfg->outputFormat = 0; // raw AAC (the MP4 carries the config in esds)
    cfg->inputFormat = FAAC_INPUT_16BIT;
    if (!faacEncSetConfiguration(p->enc, cfg) || faacEncGetDecoderSpecificInfo(p->enc, &p->asc, &p->ascLen) ||
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
    if (p->enc)
        faacEncClose(p->enc);
    free(p->asc);
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

        int n = faacEncEncode(p->enc, (int32_t*) block, AACPIPE_FRAME_SAMPLES, p->out, (unsigned int) p->maxOut);
        if (n <= 0)
            continue; // still priming
        // Output k (k >= 1 after the priming frame) holds input samples starting at (k - 1) * 1024.
        uint64_t k = p->outFrames++;
        if (k == 0)
            continue; // drop the priming frame
        int64_t ts = baseUs + (int64_t) ((k - 1) * AACPIPE_FRAME_SAMPLES * 1000000ULL / p->rate);
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
