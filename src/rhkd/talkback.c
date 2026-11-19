// SPDX-License-Identifier: GPL-3.0-or-later
// Two-way audio (see talkback.h).
//
// rad plays whatever PCM arrives in the "speaker" ring (16-bit mono at its AO rate, 16 kHz by default), the same path
// raptor's WebRTC daemon rwd uses for its backchannel: rad's AO thread opens the ring when it appears, plays each
// slot, and drops the ring after 500 ms without data. So, like rwd: open the ring if it exists, otherwise create it,
// write 20 ms chunks, and destroy it when the session ends.
//
// Opus is decoded straight to 16 kHz (libopus resamples internally). Packets arrive from the network thread of
// the session's audio sender; nothing here blocks.
#include "talkback.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "HAP.h"
#include "opus.h"
#include "rss_ipc.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "Talkback" };

#define TB_RATE        16000
#define TB_CHUNK       320   // 20 ms at 16 kHz, rad's AO frame
#define TB_MAX_SAMPLES 1920  // 120 ms at 16 kHz: the longest Opus packet
#define TB_RING_SLOTS  16
#define TB_RING_DATA   (64 * 1024)

static atomic_bool gMuted;
static _Atomic uint8_t gVolume = 100;

// Echo cancellation (Home's "Echo Cancellation" switch): while the switch is on and someone talks, rad's AEC runs, so
// the camera mic doesn't send the speaker's sound back. rhkd turns it on at the first talk-back packet and off when the
// last talking session ends -- only if rhkd turned it on (an owner who set aec_enabled in raptor.conf keeps it).
#define RAD_SOCK "/var/run/rss/rad.sock"
static pthread_mutex_t gAecLock = PTHREAD_MUTEX_INITIALIZER;
static bool gAecWanted;  // the switch
static int gAecTalkers;  // sessions currently playing talk-back
static bool gAecOurs;    // rhkd enabled rad's AEC and must turn it off again

struct talkback {
    OpusDecoder* dec;
    rss_ring_t* ring;
    uint8_t pt;
    uint32_t ssrc;
    bool haveSsrc;
    uint16_t lastSeq;
    bool haveSeq;
    uint32_t packets, lost, badPt, decodeErrors;
    uint64_t samples;
    bool logged;
    bool talking; // counted in gAecTalkers
    uint32_t busy; // packets dropped while another session held the speaker
};

// One speaker: the first session that plays holds it until it ends; a second viewer talking meanwhile is not mixed in.
static talkback_t* _Atomic gSpeakerOwner;

static bool RadAec(bool* on) {
    char resp[1024];
    if (rss_ctrl_send_command(RAD_SOCK, "{\"cmd\":\"status\"}", resp, sizeof resp, 500) <= 0)
        return false;
    const char* p = strstr(resp, "\"aec\":");
    if (!p)
        return false; // rad built without audio effects, or ao disabled
    *on = strncmp(p + 6, "true", 4) == 0;
    return true;
}

static void SetRadAec(bool on) {
    char cmd[64], resp[256];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"set-aec\",\"value\":%d}", on ? 1 : 0);
    if (rss_ctrl_send_command(RAD_SOCK, cmd, resp, sizeof resp, 1000) > 0 && strstr(resp, "\"ok\""))
        HAPLogInfo(&logObject, "echo cancellation %s", on ? "on" : "off");
    else
        HAPLogError(&logObject, "rad set-aec %d failed: %s", on ? 1 : 0, resp);
}

// Caller holds gAecLock. Brings rad's AEC in line with the switch and the talkers.
static void AecUpdate(void) {
    bool want = gAecWanted && gAecTalkers > 0;
    if (want && !gAecOurs) {
        bool on = false;
        if (!RadAec(&on)) {
            HAPLogInfo(&logObject, "echo cancellation unavailable (rad has no AEC)");
            gAecWanted = false;
            return;
        }
        if (!on) {
            SetRadAec(true);
            gAecOurs = true;
        }
    } else if (!want && gAecOurs) {
        SetRadAec(false);
        gAecOurs = false;
    }
}

void talkback_set_echo_cancel(bool on) {
    pthread_mutex_lock(&gAecLock);
    gAecWanted = on;
    AecUpdate();
    pthread_mutex_unlock(&gAecLock);
}

bool talkback_echo_cancel(void) {
    pthread_mutex_lock(&gAecLock);
    bool on = gAecWanted;
    pthread_mutex_unlock(&gAecLock);
    return on;
}

void talkback_shutdown(void) {
    pthread_mutex_lock(&gAecLock);
    gAecTalkers = 0;
    AecUpdate();
    pthread_mutex_unlock(&gAecLock);
}

static int64_t MonoUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void talkback_set_muted(bool muted) {
    atomic_store(&gMuted, muted);
}
bool talkback_muted(void) {
    return atomic_load(&gMuted);
}
void talkback_set_volume(uint8_t percent) {
    atomic_store(&gVolume, percent > 100 ? 100 : percent);
}
uint8_t talkback_volume(void) {
    return atomic_load(&gVolume);
}

talkback_t* talkback_open(uint8_t payloadType) {
    talkback_t* t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    int err = 0;
    t->dec = opus_decoder_create(TB_RATE, 1, &err);
    if (!t->dec || err != OPUS_OK) {
        HAPLogError(&logObject, "talkback: Opus decoder failed (%d)", err);
        free(t);
        return NULL;
    }
    t->pt = payloadType;
    return t;
}

static bool OpenRing(talkback_t* t) {
    t->ring = rss_ring_open("speaker"); // rad may still have one mapped
    if (!t->ring)
        t->ring = rss_ring_create("speaker", TB_RING_SLOTS, TB_RING_DATA);
    if (!t->ring)
        return false;
    rss_ring_set_stream_info(t->ring, 0x11, 0, 0, 0, TB_RATE, 1, 0, 0); // as rwd: PCM16, 16 kHz, mono
    return true;
}

static bool Publish(talkback_t* t, const int16_t* pcm, int n) {
    talkback_t* none = NULL;
    if (atomic_load(&gSpeakerOwner) != t && !atomic_compare_exchange_strong(&gSpeakerOwner, &none, t)) {
        t->busy++;
        return false;
    }
    if (!t->ring && !OpenRing(t)) {
        HAPLogError(&logObject, "talkback: can't open or create rad's speaker ring");
        return false;
    }
    for (int o = 0; o < n; o += TB_CHUNK) {
        int c = n - o < TB_CHUNK ? n - o : TB_CHUNK;
        rss_ring_publish(t->ring, (const uint8_t*) (pcm + o), (uint32_t) c * 2, MonoUs(), 0, 0, RSS_SRC_SEQ_NONE);
    }
    return true;
}

void talkback_rtp(talkback_t* t, const uint8_t* rtp, size_t len) {
    if (!t || len < 12)
        return;
    uint8_t pt = rtp[1] & 0x7F;
    if (pt != t->pt) {
        t->badPt++;
        return;
    }
    size_t hdr = 12 + 4 * (rtp[0] & 0x0F);
    if (rtp[0] & 0x10) {
        if (len < hdr + 4)
            return;
        hdr += 4 + 4 * ((rtp[hdr + 2] << 8) | rtp[hdr + 3]);
    }
    size_t end = len;
    if (rtp[0] & 0x20) { // padding
        if (rtp[len - 1] > len - hdr)
            return;
        end -= rtp[len - 1];
    }
    if (end <= hdr)
        return;
    uint16_t seq = (uint16_t) ((rtp[2] << 8) | rtp[3]);
    uint32_t ssrc = ((uint32_t) rtp[8] << 24) | ((uint32_t) rtp[9] << 16) | ((uint32_t) rtp[10] << 8) | rtp[11];
    if (!t->haveSsrc || ssrc != t->ssrc) { // new talker: reset the sequence tracking
        t->ssrc = ssrc;
        t->haveSsrc = true;
        t->haveSeq = false;
    }
    int16_t pcm[TB_MAX_SAMPLES];
    if (t->haveSeq) {
        uint16_t gap = (uint16_t) (seq - t->lastSeq);
        if (gap == 0 || gap > 0x8000)
            return; // duplicate or late
        if (gap > 1 && gap <= 5) { // a few packets lost: let Opus conceal them (one 20 ms frame each)
            for (uint16_t i = 1; i < gap; i++) {
                int n = opus_decode(t->dec, NULL, 0, pcm, TB_CHUNK, 0);
                if (n > 0 && !atomic_load(&gMuted))
                    Publish(t, pcm, n);
            }
        }
        if (gap > 1)
            t->lost += gap - 1;
    }
    t->lastSeq = seq;
    t->haveSeq = true;
    t->packets++;

    int n = opus_decode(t->dec, rtp + hdr, (opus_int32) (end - hdr), pcm, TB_MAX_SAMPLES, 0);
    if (n <= 0) {
        t->decodeErrors++;
        return;
    }
    if (!t->logged) {
        t->logged = true;
        HAPLogInfo(&logObject, "talkback: first packet from SSRC %08lx (pt %u, %d samples/packet) -> speaker",
                   (unsigned long) ssrc, pt, n);
        pthread_mutex_lock(&gAecLock);
        t->talking = true;
        gAecTalkers++;
        AecUpdate();
        pthread_mutex_unlock(&gAecLock);
    }
    if (atomic_load(&gMuted))
        return;
    unsigned vol = atomic_load(&gVolume);
    if (vol < 100) {
        for (int i = 0; i < n; i++)
            pcm[i] = (int16_t) ((int32_t) pcm[i] * (int32_t) vol / 100);
    }
    if (Publish(t, pcm, n))
        t->samples += (uint64_t) n;
}

void talkback_close(talkback_t* t) {
    if (!t)
        return;
    if (t->packets || t->badPt)
        HAPLogInfo(&logObject, "talkback: %lu packets, %llu ms played, %lu lost, %lu decode errors, %lu other payload types, "
                   "%lu while the speaker was busy",
                   (unsigned long) t->packets, (unsigned long long) (t->samples * 1000 / TB_RATE),
                   (unsigned long) t->lost, (unsigned long) t->decodeErrors, (unsigned long) t->badPt,
                   (unsigned long) t->busy);
    if (t->talking) {
        pthread_mutex_lock(&gAecLock);
        gAecTalkers--;
        AecUpdate();
        pthread_mutex_unlock(&gAecLock);
    }
    talkback_t* self = t;
    atomic_compare_exchange_strong(&gSpeakerOwner, &self, NULL);
    if (t->ring)
        rss_ring_destroy(t->ring); // as rwd: rad drops its mapping after 500 ms idle and waits for a new ring
    opus_decoder_destroy(t->dec);
    free(t);
}
