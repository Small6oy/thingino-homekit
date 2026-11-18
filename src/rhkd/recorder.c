// SPDX-License-Identifier: GPL-3.0-or-later
//
// HomeKit Secure Video recorder.
//
// Two threads run while recording is active:
//  - the reader copies every frame of raptor's main H.264 ring (RHKD_RECORD_RING, default "main") into an
//    in-memory queue. It never touches the network: the ring's data region holds well under a second of video
//    (keyframes are ~350 KB), so any stall there loses frames. With no stream open it trims the queue to start
//    at the newest keyframe at least the prebuffer length old.
//  - the sender, once a controller opens a recording, sends the fMP4 init segment, then the queue as fragments
//    that each start on a keyframe and last at least the selected fragment length, then live video the same
//    way, until the controller closes. Only the sender frees frames while streaming, so it can use them
//    unlocked; the reader only appends.
//
// HDS packet format ported from HAP-NodeJS CameraRecordingStream (src/lib/camera/RecordingManagement.ts,
// Apache-2.0, Copyright The HAP-NodeJS contributors): dataSend/data {streamId, packets: [{data, metadata:
// {dataType mediaInitialization|mediaFragment, dataSequenceNumber, dataChunkSequenceNumber, isLastDataChunk,
// dataTotalSize (first chunk)}}], endOfStream (last chunk only)}, chunks of at most 256 KiB.
//
// Audio (RecordingAudioActive): a rad ring carrying AAC-LC 16 kHz mono with 1024-sample frames ("audio", as
// Thingino ships rad, or "audio2" with rad's dual-codec output) is used as is. Otherwise the reader decodes whichever
// ring carries Opus and encodes AAC-LC itself (aacpipe.c). Either
// way the AAC frames go into a second queue, and each fragment carries those captured during its video span as
// track 2.

#include "recorder.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "HAP.h"
#include "fmp4.h"

// RHKD_TRANSCODE=0 (build/Makefile TRANSCODE=0) leaves out the Opus -> AAC fallback with libopus and faac.
#ifndef RHKD_TRANSCODE
#define RHKD_TRANSCODE 1
#endif
#if RHKD_TRANSCODE
#include "aacpipe.h"
#else
typedef struct aacpipe aacpipe_t;
#define AACPIPE_FRAME_SAMPLES 1024
#endif
#include "rss_ipc.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "Recorder" };

// Memory caps. The v4 (T41) has ~60 MB for Linux; with 8/12 MB caps rhkd peaked at 21.6 MB RSS and a second
// (test) instance got OOM-killed. A 4 s prebuffer of a 3 Mbit/s stream plus one GOP is ~2-2.5 MB.
#define IDLE_MAX_BYTES   (4u * 1024 * 1024) // prebuffer cap
#define STREAM_MAX_BYTES (6u * 1024 * 1024) // backlog cap while streaming (slow link): oldest GOPs are dropped
#define MAX_STREAM_MS    (10u * 60 * 1000)
#define CHUNK_MAX        0x40000

typedef struct frame {
    struct frame* next;
    int64_t ts; // us
    bool key;
    uint32_t len;
    uint8_t data[];
} frame_t;

static struct {
    pthread_mutex_t lock; // stream request fields
    pthread_t reader, sender;
    bool running;
    atomic_bool stop;
    uint32_t prebufferMs, fragmentMs;
    hds_conn_t* conn;
    int64_t streamId;
    bool streamNew;
    atomic_uint width, height;
    atomic_bool audioOn;     // RecordingAudioActive
    atomic_uint audioBitrate;
} r = { .lock = PTHREAD_MUTEX_INITIALIZER };

#define AUDIO_RATE 16000

typedef struct aframe {
    struct aframe* next;
    int64_t ts; // us, first sample
    uint32_t len;
    uint8_t data[];
} aframe_t;

// Queue: the reader appends, the sender (or the reader while idle) removes from the head.
static pthread_mutex_t qLock = PTHREAD_MUTEX_INITIALIZER;
static frame_t *qHead, *qTail;
static size_t qBytes;
static bool qStreaming; // sender owns the head; the reader must not trim
static aframe_t *aHead, *aTail; // AAC frames (same lock)
static uint8_t aAsc[16];
static size_t aAscLen;          // > 0 once the AAC encoder is running

#if RHKD_TRANSCODE
// The AAC encoder (created and used by the reader thread, only for the Opus fallback). faac is double precision:
// the binary's FP ABI must match the SoC (T41 = FP64/NAN2008), or it breaks on worker threads (homekit.md).
static aacpipe_t* gPipe;
#endif
// --- clock alignment -------------------------------------------------------------------------------------------------
// rad stamps audio from a sample clock that drifts against the video capture clock (seen: audio 6 s ahead after
// ~1 h on the T31). Both rings are compared with CLOCK_MONOTONIC_RAW: the smallest (now - timestamp) over a
// window is the ring clock's offset plus its minimum pipeline latency. AAC frames are moved onto the video clock
// with (audio offset - video offset). A 10 s window follows drift in either direction.
typedef struct {
    int64_t off, winMin;
    int64_t winStart;
    bool have, winHave;
} clk_t;
static clk_t vClk, aClk; // guarded by qLock

static int64_t RawUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void ClkObserve(clk_t* c, int64_t ts) { // qLock held
    int64_t now = RawUs(), d = now - ts;
    if (!c->winHave || d < c->winMin) {
        c->winMin = d;
        c->winHave = true;
    }
    if (!c->have || d < c->off)
        c->off = d, c->have = true; // fast convergence at start
    if (now - c->winStart > 10000000) { // close the window: adopt its minimum (follows drift either way)
        c->off = c->winMin;
        c->winHave = false;
        c->winStart = now;
    }
}

static uint64_t NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

// --- queue helpers (call with qLock held) ---------------------------------------------------------------------------

static void Push(frame_t* f) {
    f->next = NULL;
    if (qTail)
        qTail->next = f;
    else
        qHead = f;
    qTail = f;
    qBytes += f->len;
}

static void DropBefore(frame_t* keep) {
    while (qHead && qHead != keep) {
        frame_t* f = qHead;
        qHead = f->next;
        if (!qHead)
            qTail = NULL;
        qBytes -= f->len;
        free(f);
    }
}

static frame_t* NextKey(frame_t* f) {
    while (f && !f->key)
        f = f->next;
    return f;
}

static void ADropBefore(int64_t ts) { // drop AAC frames that start before @ts (qLock held)
    while (aHead && aHead->ts < ts) {
        aframe_t* f = aHead;
        aHead = f->next;
        if (!aHead)
            aTail = NULL;
        free(f);
    }
}

static void AClear(void) {
    ADropBefore(INT64_MAX);
}

// Keep from the newest keyframe at least @prebufferMs older than the newest frame; never start mid-GOP.
static void TrimPrebuffer(void) {
    if (!qTail)
        return;
    frame_t* start = NULL;
    for (frame_t* f = qHead; f; f = f->next)
        if (f->key && (qTail->ts - f->ts) >= (int64_t) r.prebufferMs * 1000)
            start = f;
    if (start)
        DropBefore(start);
    if (qHead && !qHead->key)
        DropBefore(NextKey(qHead));
    while (qBytes > IDLE_MAX_BYTES && qHead)
        DropBefore(NextKey(qHead->next));
    ADropBefore(qHead ? qHead->ts - 200000 : INT64_MAX);
}

// ---------------------------------------------------------------------------------------------------------------------
// Reader thread

static void OnAac(void* ctx HAP_UNUSED, const uint8_t* aac, size_t len, int64_t tsUs) {
    aframe_t* f = malloc(sizeof *f + len);
    if (!f)
        return;
    f->next = NULL;
    f->len = (uint32_t) len;
    memcpy(f->data, aac, len);
    pthread_mutex_lock(&qLock);
    f->ts = tsUs + (aClk.have && vClk.have ? aClk.off - vClk.off : 0); // onto the video clock
    if (aTail)
        aTail->next = f;
    else
        aHead = f;
    aTail = f;
    pthread_mutex_unlock(&qLock);
}

// Audio side of the reader: opens the Opus ring and the AAC encoder while RecordingAudioActive is on.
typedef struct {
    rss_ring_t* ring;
    aacpipe_t* pipe;
    uint8_t* buf;
    uint32_t max;
    uint64_t seq;
    uint64_t retryAt;
    bool direct; // reading rad's AAC ring "audio2" (no transcoding)
} audio_in_t;

// AudioSpecificConfig for AAC (@aot) at @rate Hz, mono. Returns false for a rate AAC can't signal by index.
static bool MakeAsc(unsigned aot, uint32_t rate, uint8_t out[2]) {
    static const uint32_t rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++)
        if (rates[i] == rate) {
            uint16_t v = (uint16_t) ((aot << 11) | (i << 7) | (1 << 3));
            out[0] = (uint8_t) (v >> 8);
            out[1] = (uint8_t) v;
            return true;
        }
    return false;
}

// Opens rad ring @name if it carries what HKSV selected (AAC-LC, 1024-sample frames, AUDIO_RATE), used as is.
// rad publishes "audio" and, with its dual-codec support, "audio2"; either may be the AAC one.
static bool OpenDirect(audio_in_t* a, const char* name) {
    rss_ring_t* ring = rss_ring_open(name);
    if (!ring)
        return false; // no such ring (e.g. rad without a second codec output)
    rss_stream_info_t info = { 0 };
    uint8_t asc[2];
    bool ok = rss_ring_version_ok(ring, NULL) && rss_ring_get_stream_info(ring, &info) == 0 && info.codec == 97 &&
              info.profile == 2 && info.width == AACPIPE_FRAME_SAMPLES && info.fps_num == AUDIO_RATE &&
              MakeAsc(2, info.fps_num, asc);
    if (!ok) {
        if (info.codec == 97)
            HAPLogInfo(&logObject, "%s ring is AAC but not AAC-LC %u Hz / 1024 (aot %u, %u Hz, %u samples)", name,
                       AUDIO_RATE, (unsigned) info.profile, (unsigned) info.fps_num, (unsigned) info.width);
        rss_ring_close(ring);
        return false;
    }
    a->ring = ring;
    a->direct = true;
    a->max = rss_ring_max_frame_size(ring);
    a->buf = malloc(a->max);
    if (!a->buf) {
        rss_ring_close(ring);
        a->ring = NULL;
        a->direct = false;
        return false;
    }
    rss_ring_acquire(ring);
    a->seq = atomic_load(&rss_ring_get_header(ring)->write_seq) + 1;
    pthread_mutex_lock(&qLock);
    memcpy(aAsc, asc, 2);
    aAscLen = 2;
    pthread_mutex_unlock(&qLock);
    HAPLogInfo(&logObject, "recording audio: rad AAC ring %s (AAC-LC %u Hz mono, no transcoding), ASC %02X%02X", name,
               AUDIO_RATE, asc[0], asc[1]);
    return true;
}

#if RHKD_TRANSCODE
// The first rad ring carrying Opus (for the transcoding fallback), or NULL.
static rss_ring_t* OpenOpus(const char** nameOut) {
    static const char* const names[] = { "audio2", "audio3", "audio" };
    for (unsigned i = 0; i < 3; i++) {
        rss_ring_t* ring = rss_ring_open(names[i]);
        if (!ring)
            continue;
        if (rss_ring_version_ok(ring, NULL) && rss_ring_get_header(ring)->codec == 111) {
            *nameOut = names[i];
            return ring;
        }
        rss_ring_close(ring);
    }
    return NULL;
}
#endif

static void AudioClose(audio_in_t* a) {
    if (a->ring) {
        rss_ring_release(a->ring);
        rss_ring_close(a->ring);
    }
    free(a->buf); // the pipe (gPipe) is kept for the next recording
    memset(a, 0, sizeof *a);
    pthread_mutex_lock(&qLock);
    aAscLen = 0;
    AClear();
    pthread_mutex_unlock(&qLock);
}

static void AudioPump(audio_in_t* a) {
    bool on = atomic_load(&r.audioOn);
    if (!on) {
        if (a->ring || a->pipe)
            AudioClose(a);
        return;
    }
    if (!a->ring) {
        if (NowMs() < a->retryAt)
            return;
        a->retryAt = NowMs() + 5000;
        // rad's AAC outputs, best first: "audio3" (on-demand AAC at a recording bitrate, three-output rad), the main
        // "audio" (Thingino ships it as AAC), "audio2".
        if (OpenDirect(a, "audio3") || OpenDirect(a, "audio") || OpenDirect(a, "audio2"))
            goto read;
#if !RHKD_TRANSCODE
        HAPLogError(&logObject, "no rad ring carries AAC-LC %u Hz mono (set [audio3] codec = aac in raptor.conf); "
                    "this rhkd is built without Opus transcoding: recordings stay silent", AUDIO_RATE);
        return;
#else
        const char* opusName = NULL;
        a->ring = OpenOpus(&opusName);
        if (!a->ring) {
            HAPLogError(&logObject, "no rad ring carries AAC-LC 16 kHz or Opus: recordings stay silent");
            return;
        }
        HAPLogInfo(&logObject, "recording audio: transcoding the Opus ring %s", opusName);
        // The AAC encoder is created only here, when rad has no AAC ring: faac's state is ~10 MB (per-channel
        // arrays for up to 64 channels), too much to keep around on a camera where it isn't used. Needs an FP ABI
        // matching the SoC for faac on this thread (homekit.md). Kept once created.
        if (!gPipe) {
            uint32_t bitrate = atomic_load(&r.audioBitrate);
            gPipe = aacpipe_open(AUDIO_RATE, bitrate < 24000 ? 24000 : bitrate);
        }
        a->pipe = gPipe;
        a->max = rss_ring_max_frame_size(a->ring);
        a->buf = malloc(a->max);
        if (!a->pipe || !a->buf) {
            HAPLogError(&logObject, "could not start the AAC encoder");
            AudioClose(a);
            return;
        }
        rss_ring_acquire(a->ring);
        a->seq = atomic_load(&rss_ring_get_header(a->ring)->write_seq) + 1;
        size_t n;
        const uint8_t* asc = aacpipe_asc(a->pipe, &n);
        pthread_mutex_lock(&qLock);
        aAscLen = n <= sizeof aAsc ? n : 0;
        memcpy(aAsc, asc, aAscLen);
        pthread_mutex_unlock(&qLock);
        HAPLogInfo(&logObject, "recording audio: Opus ring -> AAC-LC %u Hz mono, ASC %02X%02X", AUDIO_RATE,
                   n > 0 ? asc[0] : 0, n > 1 ? asc[1] : 0);
#endif
    }
read:
    for (;;) {
        uint32_t len = 0;
        rss_ring_slot_t meta;
        int ret = rss_ring_read(a->ring, &a->seq, a->buf, a->max, &len, &meta);
        if (ret == RSS_EOVERFLOW) {
            a->seq = atomic_load(&rss_ring_get_header(a->ring)->write_seq);
            continue;
        }
        if (ret)
            break;
        pthread_mutex_lock(&qLock);
        ClkObserve(&aClk, meta.timestamp);
        pthread_mutex_unlock(&qLock);
        if (a->direct)
            OnAac(NULL, a->buf, len, meta.timestamp); // one raw AAC-LC access unit per slot
#if RHKD_TRANSCODE
        else if (aacpipe_decode(a->pipe, a->buf, len, meta.timestamp) >= AACPIPE_FRAME_SAMPLES)
            (void) aacpipe_encode(a->pipe, OnAac, NULL);
#endif
    }
    if (rss_ring_stale(a->ring))
        AudioClose(a);
}

static void* Reader(void* arg HAP_UNUSED) {
    const char* ringName = getenv("RHKD_RECORD_RING");
    if (!ringName || !*ringName)
        ringName = "main";
    rss_ring_t* ring = NULL;
    uint8_t* buf = NULL;
    uint32_t maxFrame = 0, overflows = 0;
    uint64_t readSeq = 0;
    audio_in_t audio = { 0 };

    while (!atomic_load(&r.stop)) {
        AudioPump(&audio);
        if (!ring) {
            ring = rss_ring_open(ringName);
            if (!ring || !rss_ring_version_ok(ring, NULL) || rss_ring_get_header(ring)->codec != 0) {
                if (ring) {
                    HAPLogError(&logObject, "ring %s is not H.264; recording needs an H.264 stream", ringName);
                    rss_ring_close(ring);
                    ring = NULL;
                }
                sleep(2);
                continue;
            }
            maxFrame = rss_ring_max_frame_size(ring);
            free(buf);
            buf = malloc(maxFrame);
            rss_ring_acquire(ring);
            const rss_ring_header_t* h = rss_ring_get_header(ring);
            atomic_store(&r.width, h->width);
            atomic_store(&r.height, h->height);
            readSeq = atomic_load(&h->write_seq) + 1;
            HAPLogInfo(&logObject, "prebuffering %s (%ux%u), %u ms", ringName, h->width, h->height, r.prebufferMs);
        }
        (void) rss_ring_wait(ring, 100);
        for (;;) {
            uint32_t len = 0;
            rss_ring_slot_t meta;
            int ret = buf ? rss_ring_read(ring, &readSeq, buf, maxFrame, &len, &meta) : -1;
            if (ret == RSS_EOVERFLOW) {
                readSeq = atomic_load(&rss_ring_get_header(ring)->write_seq);
                if (++overflows <= 5 || overflows % 100 == 0)
                    HAPLogError(&logObject, "recorder fell behind the ring: frames lost (%u times)", overflows);
                continue;
            }
            if (ret)
                break;
            frame_t* f = malloc(sizeof *f + len);
            if (!f)
                break;
            f->ts = meta.timestamp;
            f->key = meta.is_key;
            f->len = len;
            memcpy(f->data, buf, len);
            pthread_mutex_lock(&qLock);
            ClkObserve(&vClk, meta.timestamp);
            Push(f);
            if (!qStreaming)
                TrimPrebuffer();
            pthread_mutex_unlock(&qLock);
        }
        if (rss_ring_stale(ring)) {
            HAPLogInfo(&logObject, "ring %s restarted; reopening", ringName);
            rss_ring_release(ring);
            rss_ring_close(ring);
            ring = NULL;
        }
    }
    if (ring) {
        rss_ring_release(ring);
        rss_ring_close(ring);
    }
    AudioClose(&audio);
    free(buf);
    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sender thread

typedef struct {
    hds_conn_t* conn;
    int64_t streamId;
    uint32_t dataSeq;
    bool failed;
    uint8_t* msg;
} sender_t;

static bool SendPacket(sender_t* s, const char* dataType, const uint8_t* data, size_t len) {
    uint32_t chunkSeq = 1;
    size_t off = 0;
    do {
        size_t n = len - off < CHUNK_MAX ? len - off : CHUNK_MAX;
        bool last = off + n >= len;
        op_writer_t w;
        op_w_init(&w, s->msg, CHUNK_MAX + 512);
        op_w_dict(&w, last ? 3 : 2);
        op_w_str(&w, "streamId");
        op_w_int(&w, s->streamId);
        op_w_str(&w, "packets");
        op_w_array(&w, 1);
        op_w_dict(&w, 2);
        op_w_str(&w, "data");
        op_w_data(&w, data + off, n);
        op_w_str(&w, "metadata");
        op_w_dict(&w, chunkSeq == 1 ? 5 : 4);
        op_w_str(&w, "dataType");
        op_w_str(&w, dataType);
        op_w_str(&w, "dataSequenceNumber");
        op_w_int(&w, s->dataSeq);
        op_w_str(&w, "dataChunkSequenceNumber");
        op_w_int(&w, chunkSeq);
        op_w_str(&w, "isLastDataChunk");
        op_w_bool(&w, last);
        if (chunkSeq == 1) {
            op_w_str(&w, "dataTotalSize");
            op_w_int(&w, (int64_t) len);
        }
        if (last) {
            op_w_str(&w, "endOfStream");
            op_w_bool(&w, false);
        }
        if (w.overflow || hds_send_event(s->conn, "dataSend", "data", w.b, w.len)) {
            s->failed = true;
            return false;
        }
        off += n;
        chunkSeq++;
    } while (off < len);
    s->dataSeq++;
    return true;
}

// Sends every complete fragment in the queue: a run of frames from a keyframe up to (not including) the first
// later keyframe at least fragmentMs after it, so every sample's duration is known.
typedef struct {
    bool on;          // the init segment has an audio track
    bool started;
    uint64_t next;    // decode time of the next AAC frame (AUDIO_RATE units)
    uint32_t frames;
} audio_out_t;

static bool EmitFragments(sender_t* s, uint32_t* fragSeq, int64_t* t0, uint32_t* frames, uint32_t* dropped,
                          audio_out_t* ao) {
    for (;;) {
        pthread_mutex_lock(&qLock);
        while (qBytes > STREAM_MAX_BYTES && qHead) { // link too slow: drop the oldest GOP
            frame_t* k = NextKey(qHead->next);
            for (frame_t* f = qHead; f != k; f = f->next)
                (*dropped)++;
            DropBefore(k);
        }
        if (qHead && !qHead->key)
            DropBefore(NextKey(qHead));
        frame_t *head = qHead, *end = NULL;
        size_t count = 0;
        if (head)
            for (frame_t* f = head; f; f = f->next) {
                if (f != head && f->key && f->ts - head->ts >= (int64_t) r.fragmentMs * 1000) {
                    end = f;
                    break;
                }
                count++;
            }
        pthread_mutex_unlock(&qLock);
        if (!end)
            return true;

        // The frames head..end are ours until we drop them (the reader only appends).
        fmp4_sample_t* smp = malloc(count * sizeof *smp);
        if (!smp)
            return false;
        size_t i = 0;
        for (frame_t* f = head; f != end; f = f->next, i++) {
            int64_t dur = (f->next->ts - f->ts) * FMP4_TIMESCALE / 1000000;
            smp[i] = (fmp4_sample_t) { .data = f->data, .len = f->len, .key = f->key,
                                       .duration = (uint32_t) (dur > 0 && dur < FMP4_TIMESCALE ? dur
                                                                                               : FMP4_TIMESCALE / 25) };
        }
        if (*t0 < 0)
            *t0 = head->ts;
        uint64_t base = (uint64_t) (head->ts - *t0) * FMP4_TIMESCALE / 1000000;

        // Audio captured before this fragment's end (older leftovers before its start are dropped).
        aframe_t* aList = NULL;
        fmp4_audio_t* aud = NULL;
        size_t acount = 0;
        if (ao->on) {
            pthread_mutex_lock(&qLock);
            ADropBefore(ao->started ? INT64_MIN : head->ts - 32000);
            aframe_t** tail = &aList;
            while (aHead && aHead->ts < end->ts) {
                aframe_t* f = aHead;
                aHead = f->next;
                if (!aHead)
                    aTail = NULL;
                f->next = NULL;
                *tail = f;
                tail = &f->next;
                acount++;
            }
            pthread_mutex_unlock(&qLock);
            if (acount && (aud = malloc(acount * sizeof *aud))) {
                size_t k = 0;
                for (aframe_t* f = aList; f; f = f->next)
                    aud[k++] = (fmp4_audio_t) { .data = f->data, .len = f->len };
                if (!ao->started) {
                    int64_t off = aList->ts - *t0;
                    ao->next = off > 0 ? (uint64_t) off * AUDIO_RATE / 1000000 : 0;
                    ao->started = true;
                }
            } else {
                acount = 0;
            }
        }
        fmp4_buf_t o = { 0 };
        bool ok = fmp4_fragment(&o, (*fragSeq)++, base, smp, count, aud, acount, ao->next) &&
                  SendPacket(s, "mediaFragment", o.b, o.len);
        ao->next += (uint64_t) acount * AACPIPE_FRAME_SAMPLES;
        ao->frames += (uint32_t) acount;
        free(aud);
        while (aList) {
            aframe_t* n = aList->next;
            free(aList);
            aList = n;
        }
        free(smp);
        HAPLogDebug(&logObject, "fragment %u: %zu frames, %zu bytes", *fragSeq - 1, count, o.len);
        fmp4_buf_free(&o);
        *frames += (uint32_t) count;
        pthread_mutex_lock(&qLock);
        DropBefore(end);
        pthread_mutex_unlock(&qLock);
        if (!ok)
            return false;
    }
}

static void* Sender(void* arg HAP_UNUSED) {
    sender_t s = { .msg = malloc(CHUNK_MAX + 512) };
    bool streaming = false, sentInit = false;
    uint32_t fragSeq = 1, frames = 0, dropped = 0;
    int64_t t0 = -1;
    uint64_t streamStart = 0;
    audio_out_t ao = { 0 };

    while (!atomic_load(&r.stop) && s.msg) {
        pthread_mutex_lock(&r.lock);
        hds_conn_t* conn = r.conn;
        bool isNew = r.streamNew;
        r.streamNew = false;
        if (isNew) {
            s.conn = conn;
            s.streamId = r.streamId;
        }
        pthread_mutex_unlock(&r.lock);

        if (isNew) {
            streaming = true;
            sentInit = false;
            s.dataSeq = 1;
            s.failed = false;
            fragSeq = 1;
            frames = dropped = 0;
            t0 = -1;
            streamStart = NowMs();
            pthread_mutex_lock(&qLock);
            qStreaming = true;
            unsigned queued = qHead ? (unsigned) ((qTail->ts - qHead->ts) / 1000) : 0;
            pthread_mutex_unlock(&qLock);
            HAPLogInfo(&logObject, "recording stream %lld: %u ms queued", (long long) s.streamId, queued);
        } else if (streaming && conn != s.conn) {
            HAPLogInfo(&logObject, "recording stream %lld ended: %u fragments, %u frames, %u dropped, %u AAC frames, %u s",
                       (long long) s.streamId, fragSeq - 1, frames, dropped, ao.frames,
                       (unsigned) ((NowMs() - streamStart) / 1000));
            streaming = false;
            pthread_mutex_lock(&qLock);
            qStreaming = false;
            TrimPrebuffer();
            pthread_mutex_unlock(&qLock);
        }

        if (!streaming) {
            usleep(100 * 1000);
            continue;
        }

        if (!sentInit) {
            pthread_mutex_lock(&qLock);
            if (qHead && !qHead->key)
                DropBefore(NextKey(qHead));
            frame_t* k = qHead;
            pthread_mutex_unlock(&qLock);
            const uint8_t *sps, *pps;
            size_t spsLen, ppsLen;
            if (!k || !fmp4_find_params(k->data, k->len, &sps, &spsLen, &pps, &ppsLen)) {
                usleep(50 * 1000); // wait for a keyframe with SPS/PPS
                continue;
            }
            uint16_t w = (uint16_t) atomic_load(&r.width), h = (uint16_t) atomic_load(&r.height);
            uint8_t asc[16];
            pthread_mutex_lock(&qLock);
            size_t ascLen = atomic_load(&r.audioOn) ? aAscLen : 0;
            memcpy(asc, aAsc, ascLen);
            pthread_mutex_unlock(&qLock);
            ao = (audio_out_t) { .on = ascLen > 0 };
            pthread_mutex_lock(&qLock);
            HAPLogInfo(&logObject, "clocks: video head %lld us, audio queue %lld..%lld us (aligned; audio-video offset %lld ms)",
                       (long long) k->ts, aHead ? (long long) aHead->ts : -1LL, aTail ? (long long) aTail->ts : -1LL,
                       aClk.have && vClk.have ? (long long) ((aClk.off - vClk.off) / 1000) : 0LL);
            pthread_mutex_unlock(&qLock);
            fmp4_buf_t o = { 0 };
            bool ok = fmp4_init_segment(&o, w, h, sps, spsLen, pps, ppsLen, ascLen ? asc : NULL, ascLen, AUDIO_RATE) &&
                      SendPacket(&s, "mediaInitialization", o.b, o.len);
            HAPLogInfo(&logObject, "init segment %zu bytes (%ux%u, profile %u level %u, %s)%s", o.len, w, h, sps[1],
                       sps[3], ascLen ? "AAC-LC 16 kHz" : "no audio", ok ? "" : " - send failed");
            fmp4_buf_free(&o);
            sentInit = true;
            if (!ok)
                s.failed = true;
        }
        if (!s.failed && !EmitFragments(&s, &fragSeq, &t0, &frames, &dropped, &ao))
            s.failed = true;
        if (s.failed || NowMs() - streamStart > MAX_STREAM_MS) {
            HAPLogInfo(&logObject, "recording stream %lld stopped (%s)", (long long) s.streamId,
                       s.failed ? "send failed" : "maximum length");
            recorder_stop(s.conn, !s.failed, s.failed ? 5 : 0);
        }
        usleep(50 * 1000);
    }
    free(s.msg);
    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------------
// API

void recorder_set_active(bool on, uint32_t prebufferMs, uint32_t fragmentMs) {
    r.prebufferMs = prebufferMs ? prebufferMs : 4000;
    r.fragmentMs = fragmentMs ? fragmentMs : 4000;
    if (on && !r.running) {
        atomic_store(&r.stop, false);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1024 * 1024); // faac and Opus (VAR_ARRAYS) need far more than 128 KB
        bool ok = pthread_create(&r.reader, &attr, Reader, NULL) == 0;
        if (ok && pthread_create(&r.sender, &attr, Sender, NULL) != 0) {
            atomic_store(&r.stop, true);
            pthread_join(r.reader, NULL);
            ok = false;
        }
        pthread_attr_destroy(&attr);
        r.running = ok;
        if (!ok)
            HAPLogError(&logObject, "could not start the recorder threads");
    } else if (!on && r.running) {
        recorder_stop(NULL, true, 1); // not allowed any more
        atomic_store(&r.stop, true);
        pthread_join(r.sender, NULL);
        pthread_join(r.reader, NULL);
        r.running = false;
        pthread_mutex_lock(&qLock);
        qStreaming = false;
        DropBefore(NULL);
        AClear();
        pthread_mutex_unlock(&qLock);
        HAPLogInfo(&logObject, "prebuffer stopped");
    }
}

void recorder_set_audio(bool on, uint32_t bitrate) {
    atomic_store(&r.audioBitrate, bitrate);
    atomic_store(&r.audioOn, on); // the AAC encoder, if needed at all, is created by the reader (AudioPump)
}

bool recorder_busy(void) {
    pthread_mutex_lock(&r.lock);
    bool busy = r.conn != NULL;
    pthread_mutex_unlock(&r.lock);
    return busy;
}

bool recorder_start(hds_conn_t* conn, int64_t streamId) {
    if (!r.running)
        return false;
    pthread_mutex_lock(&r.lock);
    r.conn = conn;
    r.streamId = streamId;
    r.streamNew = true;
    pthread_mutex_unlock(&r.lock);
    return true;
}

void recorder_stop(hds_conn_t* conn, bool sendClose, int reason) {
    pthread_mutex_lock(&r.lock);
    hds_conn_t* c = r.conn;
    int64_t sid = r.streamId;
    if (c && (!conn || conn == c))
        r.conn = NULL;
    else
        c = NULL;
    pthread_mutex_unlock(&r.lock);
    if (c && sendClose) {
        uint8_t b[48];
        op_writer_t w;
        op_w_init(&w, b, sizeof b);
        op_w_dict(&w, 2);
        op_w_str(&w, "streamId");
        op_w_int(&w, sid);
        op_w_str(&w, "reason");
        op_w_int(&w, reason);
        (void) hds_send_event(c, "dataSend", "close", b, w.len);
    }
}
