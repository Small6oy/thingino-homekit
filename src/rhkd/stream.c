// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream.h"
#include "srtp.h"
#include "rss_ipc.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "HAP.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "Stream" };

#define RTCP_TIMEOUT_MS 30000
#define SR_INTERVAL_MS 1000
#define PKT_MAX 1600

struct stream {
    stream_params_t p;
    pthread_t thread;
    atomic_bool stop;
    atomic_bool finished;
    atomic_bool wantIdr;
    srtp_ctx_t srtp;
    uint16_t seq;
    uint32_t tsOffset;
    uint32_t lastRtpTs;
    uint32_t pktCount, octetCount;
    uint8_t sps[256], pps[64];
    size_t spsLen, ppsLen;
    // Capture-clock calibration for RTCP SR: offset = min(raw_now - frame_ts), i.e. the frame clock in raw time.
    int64_t vClkOff, vLastMetaUs;
    bool vHaveClk;
    int64_t aClkOff, aLastMetaUs;
    bool aHaveClk;

    // Audio
    pthread_t audioThread;
    bool audioThreadStarted;
    atomic_bool audioMuted;
    srtp_ctx_t audioSrtp;
    bool audioSrtpInit;
    uint16_t audioSeq;
    uint32_t audioTsOffset;
    uint32_t audioLastRtpTs;
    uint32_t audioPktCount, audioOctetCount;
    bool audioHaveTs;
    uint32_t audioNextTs;            // RTP ts of the next packet: advances by exactly the samples sent
    _Atomic uint64_t audioLastRx;    // last RTCP from the controller on the audio socket (ms)
    // Send-timing diagnostics (audio): gaps between consecutive packets over a 5 s window.
    uint64_t audioLastSendUs, audioStatStartUs;
    uint32_t audioGapMaxUs, audioGapsOver40, audioBurstsUnder2, audioStatPkts;
};

static int64_t raw_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// Tracks the smallest (raw_now - frame timestamp): the capture clock's offset plus the minimum pipeline latency.
static void clk_observe(int64_t* off, bool* have, int64_t metaUs) {
    int64_t d = raw_us() - metaUs;
    if (!*have || d < *off) {
        *off = d;
        *have = true;
    }
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------------------------------------------------------------
// Incoming RTCP (receiver reports from the controller): decrypt and log loss/jitter now and then.

static void log_rtcp_reports(const char* what, const uint8_t* d, size_t len, unsigned clockHz) {
    size_t o = 0;
    while (o + 8 <= len) {
        uint8_t rc = d[o] & 0x1F, pt = d[o + 1];
        size_t plen = ((size_t)((d[o + 2] << 8) | d[o + 3]) + 1) * 4;
        if (o + plen > len)
            break;
        size_t rb = 0;
        if (pt == 200)
            rb = o + 28; // SR: header 8 + sender info 20
        else if (pt == 201)
            rb = o + 8;
        if (rb) {
            for (unsigned i = 0; i < rc && rb + 24 <= o + plen; i++, rb += 24) {
                unsigned fraction = d[rb + 4];
                int32_t cum = (int32_t)(((uint32_t) d[rb + 5] << 16) | ((uint32_t) d[rb + 6] << 8) | d[rb + 7]);
                if (cum & 0x800000)
                    cum -= 0x1000000;
                uint32_t jitter = ((uint32_t) d[rb + 12] << 24) | ((uint32_t) d[rb + 13] << 16) |
                                  ((uint32_t) d[rb + 14] << 8) | d[rb + 15];
                HAPLogInfo(&logObject, "%s RR: lost %u.%u%% (cumulative %ld), jitter %lu ms", what,
                           fraction * 100 / 256, (fraction * 1000 / 256) % 10, (long) cum,
                           (unsigned long)(clockHz ? (uint64_t) jitter * 1000 / clockHz : 0));
            }
        }
        o += plen;
    }
}

// Drains a socket; returns true if anything arrived. Logs at most one report every 5 s.
static bool drain_rtcp(int sock, srtp_ctx_t* srtp, const char* what, unsigned clockHz, uint64_t* lastLog) {
    uint8_t rx[1500];
    bool any = false;
    ssize_t n;
    while ((n = recv(sock, rx, sizeof rx, MSG_DONTWAIT)) > 0) {
        any = true;
        if (n < 8 || rx[1] < 200 || rx[1] > 204) // not RTCP
            continue;
        size_t plain = srtp ? srtp_unprotect_rtcp(srtp, rx, (size_t) n) : 0;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (plain && now - *lastLog >= 5000) {
            *lastLog = now;
            log_rtcp_reports(what, rx, plain, clockHz);
        }
    }
    return any;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void send_pkt(stream_t* s, uint8_t* pkt, size_t len) {
    size_t n = srtp_protect_rtp(&s->srtp, pkt, len);
    if (!n)
        return;
    if (sendto(s->p.videoSock, pkt, n, 0, (struct sockaddr*) &s->p.videoDst, s->p.videoDstLen) < 0) {
        if (errno != EAGAIN && errno != ENOBUFS)
            HAPLogError(&logObject, "sendto: %d", errno);
    }
    s->pktCount++;
    s->octetCount += (uint32_t)(len - 12);
}

static size_t rtp_header(stream_t* s, uint8_t* pkt, bool marker, uint32_t ts) {
    pkt[0] = 0x80;
    pkt[1] = (uint8_t)((marker ? 0x80 : 0) | (s->p.payloadType & 0x7F));
    pkt[2] = (uint8_t)(s->seq >> 8);
    pkt[3] = (uint8_t) s->seq;
    s->seq++;
    pkt[4] = (uint8_t)(ts >> 24);
    pkt[5] = (uint8_t)(ts >> 16);
    pkt[6] = (uint8_t)(ts >> 8);
    pkt[7] = (uint8_t) ts;
    uint32_t ssrc = s->p.videoSSRC;
    pkt[8] = (uint8_t)(ssrc >> 24);
    pkt[9] = (uint8_t)(ssrc >> 16);
    pkt[10] = (uint8_t)(ssrc >> 8);
    pkt[11] = (uint8_t) ssrc;
    return 12;
}

// Sends one NAL unit (without start code) as single-NAL or FU-A packets.
static void send_nal(stream_t* s, const uint8_t* nal, size_t len, bool lastOfAU, uint32_t ts) {
    uint8_t pkt[PKT_MAX + SRTP_AUTH_TAG_LEN];
    size_t maxPayload = (size_t) s->p.mtu - 12 - SRTP_AUTH_TAG_LEN;
    if (maxPayload > PKT_MAX - 12)
        maxPayload = PKT_MAX - 12;
    if (len <= maxPayload) {
        size_t h = rtp_header(s, pkt, lastOfAU, ts);
        memcpy(pkt + h, nal, len);
        send_pkt(s, pkt, h + len);
        return;
    }
    uint8_t ind = (uint8_t)((nal[0] & 0xE0) | 28);
    uint8_t type = nal[0] & 0x1F;
    const uint8_t* p = nal + 1;
    size_t left = len - 1;
    bool start = true;
    while (left > 0) {
        size_t chunk = left > maxPayload - 2 ? maxPayload - 2 : left;
        bool end = chunk == left;
        size_t h = rtp_header(s, pkt, end && lastOfAU, ts);
        pkt[h] = ind;
        pkt[h + 1] = (uint8_t)((start ? 0x80 : 0) | (end ? 0x40 : 0) | type);
        memcpy(pkt + h + 2, p, chunk);
        send_pkt(s, pkt, h + 2 + chunk);
        p += chunk;
        left -= chunk;
        start = false;
    }
}

// Splits an Annex B access unit into NAL units.
typedef struct {
    const uint8_t* p;
    size_t n;
} nal_t;

static size_t split_annexb(const uint8_t* d, size_t len, nal_t* out, size_t max) {
    size_t count = 0, i = 0, start = SIZE_MAX;
    while (i + 3 <= len) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (i + 4 <= len && d[i + 2] == 0 && d[i + 3] == 1))) {
            size_t sc = d[i + 2] == 1 ? 3 : 4;
            if (start != SIZE_MAX && count < max) {
                size_t end = i;
                while (end > start && d[end - 1] == 0) // trailing zero bytes belong to the next start code
                    end--;
                out[count++] = (nal_t) { d + start, end - start };
            }
            i += sc;
            start = i;
        } else {
            i++;
        }
    }
    if (start != SIZE_MAX && start < len && count < max)
        out[count++] = (nal_t) { d + start, len - start };
    return count;
}

static void send_frame(stream_t* s, const uint8_t* d, size_t len, int64_t tsUs) {
    nal_t nals[32];
    size_t n = split_annexb(d, len, nals, 32);
    uint32_t ts = (uint32_t)((uint64_t) tsUs * 9 / 100) + s->tsOffset;
    s->lastRtpTs = ts;
    clk_observe(&s->vClkOff, &s->vHaveClk, tsUs);
    s->vLastMetaUs = tsUs;

    bool hasIdr = false, hasSps = false;
    for (size_t i = 0; i < n; i++) {
        uint8_t t = nals[i].p[0] & 0x1F;
        if (t == 7 && nals[i].n <= sizeof s->sps) {
            memcpy(s->sps, nals[i].p, nals[i].n);
            s->spsLen = nals[i].n;
            hasSps = true;
        } else if (t == 8 && nals[i].n <= sizeof s->pps) {
            memcpy(s->pps, nals[i].p, nals[i].n);
            s->ppsLen = nals[i].n;
        } else if (t == 5) {
            hasIdr = true;
        }
    }
    if (hasIdr && !hasSps && s->spsLen && s->ppsLen) {
        send_nal(s, s->sps, s->spsLen, false, ts);
        send_nal(s, s->pps, s->ppsLen, false, ts);
    }
    // Find the last NAL we actually send, for the marker bit.
    ssize_t last = -1;
    for (size_t i = 0; i < n; i++) {
        uint8_t t = nals[i].p[0] & 0x1F;
        if (t != 9 && t != 6 && nals[i].n > 0)
            last = (ssize_t) i;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t t = nals[i].p[0] & 0x1F;
        if (t == 9 || t == 6 || nals[i].n == 0) // drop AUD and SEI
            continue;
        send_nal(s, nals[i].p, nals[i].n, (ssize_t) i == last, ts);
    }
}

static void send_rtcp_sr_raw(int sock, const struct sockaddr_storage* dst, socklen_t dstLen, srtp_ctx_t* srtp,
                             uint32_t ssrc, uint32_t rtpTs, uint32_t pktCount, uint32_t octetCount) {
    uint8_t pkt[128];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ntpSec = (uint64_t) tv.tv_sec + 2208988800ULL;
    uint64_t ntpFrac = ((uint64_t) tv.tv_usec << 32) / 1000000;
    size_t o = 0;
    // SR
    pkt[o++] = 0x80;
    pkt[o++] = 200;
    pkt[o++] = 0;
    pkt[o++] = 6;
#define PUT32(v) \
    do { \
        uint32_t _v = (v); \
        pkt[o++] = (uint8_t)(_v >> 24); \
        pkt[o++] = (uint8_t)(_v >> 16); \
        pkt[o++] = (uint8_t)(_v >> 8); \
        pkt[o++] = (uint8_t) _v; \
    } while (0)
    PUT32(ssrc);
    PUT32((uint32_t) ntpSec);
    PUT32((uint32_t) ntpFrac);
    PUT32(rtpTs);
    PUT32(pktCount);
    PUT32(octetCount);
    // SDES CNAME
    const char* cname = "rhkd";
    size_t cl = strlen(cname);
    size_t sdesStart = o;
    pkt[o++] = 0x81;
    pkt[o++] = 202;
    pkt[o++] = 0;
    pkt[o++] = 0; // length filled below
    PUT32(ssrc);
    pkt[o++] = 1;
    pkt[o++] = (uint8_t) cl;
    memcpy(pkt + o, cname, cl);
    o += cl;
    pkt[o++] = 0; // END
    while ((o - sdesStart) % 4)
        pkt[o++] = 0;
    pkt[sdesStart + 3] = (uint8_t)((o - sdesStart) / 4 - 1);
#undef PUT32
    size_t n = srtp_protect_rtcp(srtp, pkt, o);
    if (n)
        sendto(sock, pkt, n, 0, (const struct sockaddr*) dst, dstLen);
}

// The SR must pair the wall clock with the RTP time *of that same instant*. Using the last frame's timestamp
// instead makes the mapping wobble by the frame's age (up to ~150 ms after a big keyframe), and the receiver
// keeps re-syncing audio to video: audible as audio cutting in and out.
static void send_rtcp_sr(stream_t* s) {
    uint32_t rtpNow = s->lastRtpTs;
    if (s->vHaveClk) {
        int64_t age = raw_us() - (s->vLastMetaUs + s->vClkOff);
        if (age > 0 && age < 5000000)
            rtpNow += (uint32_t)(age * 9 / 100);
    }
    send_rtcp_sr_raw(s->p.videoSock, &s->p.videoDst, s->p.videoDstLen, &s->srtp, s->p.videoSSRC, rtpNow,
                     s->pktCount, s->octetCount);
}

// ---------------------------------------------------------------------------------------------------------------------
// Audio: Opus frames from rad's ring, one RTP packet per packet time. RTP clock = p.audioClockHz (see camera.c).


// Opus frame duration in 48 kHz samples from the TOC byte (RFC 6716 section 3.1).
static unsigned opus_frame_samples48(uint8_t toc) {
    unsigned config = toc >> 3;
    if (config < 12) { // SILK: 10, 20, 40, 60 ms
        static const unsigned ms[] = { 10, 20, 40, 60 };
        return ms[config & 3] * 48;
    }
    if (config < 16) // Hybrid: 10, 20 ms
        return (config & 1) ? 960 : 480;
    static const unsigned celt[] = { 120, 240, 480, 960 }; // CELT: 2.5, 5, 10, 20 ms
    return celt[config & 3];
}

typedef struct {
    uint8_t toc;
    uint8_t data[1500];
    size_t len[6];
    size_t total;
    unsigned count;
    unsigned samples48;
    int64_t firstTsUs;
} opus_agg_t;

static void send_audio_packet(stream_t* s, const uint8_t* payload, size_t len, uint32_t ts) {
    uint8_t pkt[12 + 1500 + SRTP_AUTH_TAG_LEN];
    if (len > 1500)
        return;
    pkt[0] = 0x80;
    pkt[1] = (uint8_t)(s->p.audioPayloadType & 0x7F);
    pkt[2] = (uint8_t)(s->audioSeq >> 8);
    pkt[3] = (uint8_t) s->audioSeq;
    s->audioSeq++;
    pkt[4] = (uint8_t)(ts >> 24);
    pkt[5] = (uint8_t)(ts >> 16);
    pkt[6] = (uint8_t)(ts >> 8);
    pkt[7] = (uint8_t) ts;
    uint32_t ssrc = s->p.audioSSRC;
    pkt[8] = (uint8_t)(ssrc >> 24);
    pkt[9] = (uint8_t)(ssrc >> 16);
    pkt[10] = (uint8_t)(ssrc >> 8);
    pkt[11] = (uint8_t) ssrc;
    memcpy(pkt + 12, payload, len);
    size_t n = srtp_protect_rtp(&s->audioSrtp, pkt, 12 + len);
    if (!n)
        return;
    if (sendto(s->p.audioSock, pkt, n, 0, (struct sockaddr*) &s->p.audioDst, s->p.audioDstLen) < 0) {
        if (errno != EAGAIN && errno != ENOBUFS)
            HAPLogError(&logObject, "audio sendto: %d", errno);
    }
    s->audioLastRtpTs = ts;
    uint64_t t = now_us();
    if (s->audioLastSendUs) {
        uint32_t gap = (uint32_t)(t - s->audioLastSendUs);
        if (gap > s->audioGapMaxUs)
            s->audioGapMaxUs = gap;
        if (gap > 40000)
            s->audioGapsOver40++;
        if (gap < 2000)
            s->audioBurstsUnder2++;
    } else {
        s->audioStatStartUs = t;
    }
    s->audioLastSendUs = t;
    s->audioStatPkts++;
    if (t - s->audioStatStartUs >= 5000000) {
        HAPLogInfo(&logObject, "audio send: %u pkts in 5 s, max gap %u ms, gaps>40ms %u, back-to-back(<2ms) %u",
                   s->audioStatPkts, s->audioGapMaxUs / 1000, s->audioGapsOver40, s->audioBurstsUnder2);
        s->audioStatStartUs = t;
        s->audioGapMaxUs = s->audioGapsOver40 = s->audioBurstsUnder2 = s->audioStatPkts = 0;
    }
    s->audioPktCount++;
    s->audioOctetCount += (uint32_t) len;
}

// RTP timestamp for a packet captured at @tsUs carrying @samples48 samples (48 kHz). Consecutive packets advance by
// exactly their duration; rad's capture clock jitters by tens of microseconds per frame, and timestamps taken
// straight from it make the receiver see tiny gaps and overlaps. Resync only on a real gap (> 60 ms).
static uint32_t audio_rtp_ts(stream_t* s, int64_t tsUs, unsigned samples48) {
    uint32_t clk = s->p.audioClockHz;
    uint32_t fromClock = (uint32_t)((uint64_t) tsUs * (clk / 1000) / 1000) + s->audioTsOffset;
    int32_t drift = (int32_t)(fromClock - s->audioNextTs);
    int32_t maxDrift = (int32_t)(clk * 60 / 1000);
    if (!s->audioHaveTs || drift > maxDrift || drift < -maxDrift) {
        if (s->audioHaveTs)
            HAPLogInfo(&logObject, "audio: resync RTP timestamp (drift %ld samples)", (long) drift);
        s->audioNextTs = fromClock;
        s->audioHaveTs = true;
    }
    uint32_t ts = s->audioNextTs;
    s->audioNextTs += (uint32_t)((uint64_t) samples48 * clk / 48000);
    clk_observe(&s->aClkOff, &s->aHaveClk, tsUs);
    s->aLastMetaUs = tsUs;
    return ts;
}

// Emits the aggregated frames as one Opus packet: code 0 for a single frame, code 3 (VBR) for several.
static void flush_opus(stream_t* s, opus_agg_t* a) {
    if (!a->count)
        return;
    uint32_t ts = audio_rtp_ts(s, a->firstTsUs, a->samples48);
    uint8_t out[1600];
    size_t o = 0;
    if (a->count == 1) {
        out[o++] = a->toc;
    } else {
        out[o++] = (uint8_t)((a->toc & 0xFC) | 3);
        out[o++] = (uint8_t)(0x80 | a->count); // VBR, no padding
        for (unsigned i = 0; i + 1 < a->count; i++) {
            size_t l = a->len[i];
            if (l < 252) {
                out[o++] = (uint8_t) l;
            } else {
                out[o++] = (uint8_t)(252 + ((l - 252) & 3));
                out[o++] = (uint8_t)((l - 252) >> 2);
            }
        }
    }
    if (o + a->total <= sizeof out) {
        memcpy(out + o, a->data, a->total);
        send_audio_packet(s, out, o + a->total, ts);
    }
    a->count = 0;
    a->total = 0;
    a->samples48 = 0;
}

static void* audio_thread(void* arg) {
    stream_t* s = arg;
    rss_ring_t* ring = rss_ring_open(s->p.audioRingName);
    uint8_t* frame = NULL;
    if (!ring) {
        HAPLogError(&logObject, "cannot open audio ring %s", s->p.audioRingName);
        return NULL;
    }
    if (!rss_ring_version_ok(ring, NULL)) {
        HAPLogError(&logObject, "audio ring %s version mismatch", s->p.audioRingName);
        rss_ring_close(ring);
        return NULL;
    }
    uint32_t maxFrame = rss_ring_max_frame_size(ring);
    frame = malloc(maxFrame);
    if (!frame) {
        rss_ring_close(ring);
        return NULL;
    }
    rss_ring_acquire(ring);
    const rss_ring_header_t* hdr = rss_ring_get_header(ring);
    // Only Opus can go out (HomeKit negotiates Opus; we don't transcode). With rad on AAC or PCM the frames would
    // be sent as Opus garbage, so stay silent and say why. 111 = RAD_CODEC_OPUS.
    if (hdr->codec != 111) {
        HAPLogError(&logObject, "audio: rad ring codec is %u, not Opus (111); set [audio] codec = opus in "
                    "raptor.conf for HomeKit audio. Audio disabled for this stream.", (unsigned) hdr->codec);
        free(frame);
        rss_ring_release(ring);
        rss_ring_close(ring);
        return NULL;
    }
    uint64_t readSeq = atomic_load(&hdr->write_seq) + 1;
    unsigned wantSamples48 = (unsigned) s->p.audioPacketMs * 48;
    if (!wantSamples48)
        wantSamples48 = 960;
    opus_agg_t agg = { 0 };
    uint64_t lastSr = 0;
    uint32_t frames = 0;
    bool loggedToc = false;

    uint64_t lastRrLog = 0;
    while (!atomic_load(&s->stop)) {
        if (drain_rtcp(s->p.audioSock, &s->audioSrtp, "audio", s->p.audioClockHz, &lastRrLog))
            atomic_store(&s->audioLastRx, now_ms());
        uint32_t len = 0;
        rss_ring_slot_t meta;
        int ret = rss_ring_read(ring, &readSeq, frame, maxFrame, &len, &meta);
        if (ret == -EAGAIN) {
            rss_ring_wait(ring, 100);
        } else if (ret == RSS_EOVERFLOW) {
            readSeq = atomic_load(&hdr->write_seq);
            agg.count = 0;
            agg.total = 0;
            agg.samples48 = 0;
        } else if (ret == 0 && len >= 1) {
            frames++;
            uint8_t toc = frame[0];
            unsigned fs = opus_frame_samples48(toc);
            if (!loggedToc) {
                loggedToc = true;
                HAPLogInfo(&logObject, "audio: first Opus frame toc=0x%02x (%u ms, code %u), packet time %u ms",
                           toc, fs / 48, toc & 3, (unsigned) s->p.audioPacketMs);
            }
            if (atomic_load(&s->audioMuted)) {
                s->audioHaveTs = false;
                agg.count = 0;
                agg.total = 0;
                agg.samples48 = 0;
            } else if ((toc & 3) != 0 || wantSamples48 <= fs) {
                // Already multi-frame, or one frame fills the packet time: send as is.
                flush_opus(s, &agg);
                unsigned pkt48 = fs;
                if ((toc & 3) == 1 || (toc & 3) == 2)
                    pkt48 = fs * 2;
                else if ((toc & 3) == 3 && len >= 2)
                    pkt48 = fs * (frame[1] & 0x3F);
                send_audio_packet(s, frame, len, audio_rtp_ts(s, meta.timestamp, pkt48));
            } else {
                // Aggregate single-frame packets with the same config into one packet per packet time.
                if (agg.count && ((agg.toc & 0xFC) != (toc & 0xFC) || agg.count == 6 ||
                                  agg.total + len - 1 > sizeof agg.data))
                    flush_opus(s, &agg);
                if (!agg.count) {
                    agg.toc = toc;
                    agg.firstTsUs = meta.timestamp;
                }
                memcpy(agg.data + agg.total, frame + 1, len - 1);
                agg.len[agg.count++] = len - 1;
                agg.total += len - 1;
                agg.samples48 += fs;
                if (agg.samples48 >= wantSamples48)
                    flush_opus(s, &agg);
            }
        }
        uint64_t now = now_ms();
        if (s->audioPktCount && now - lastSr >= SR_INTERVAL_MS) {
            uint32_t rtpNow = s->audioLastRtpTs;
            if (s->aHaveClk) {
                int64_t age = raw_us() - (s->aLastMetaUs + s->aClkOff);
                if (age > 0 && age < 5000000)
                    rtpNow += (uint32_t)(age * (s->p.audioClockHz / 1000) / 1000);
            }
            send_rtcp_sr_raw(s->p.audioSock, &s->p.audioDst, s->p.audioDstLen, &s->audioSrtp, s->p.audioSSRC,
                             rtpNow, s->audioPktCount, s->audioOctetCount);
            lastSr = now;
        }
    }
    HAPLogInfo(&logObject, "audio ended after %u frames, %u packets", frames, s->audioPktCount);
    rss_ring_release(ring);
    free(frame);
    rss_ring_close(ring);
    return NULL;
}

static void* stream_thread(void* arg) {
    stream_t* s = arg;
    rss_ring_t* ring = rss_ring_open(s->p.ringName);
    uint8_t* frame = NULL;
    if (!ring) {
        HAPLogError(&logObject, "cannot open ring %s", s->p.ringName);
        goto out;
    }
    if (!rss_ring_version_ok(ring, NULL)) {
        HAPLogError(&logObject, "ring %s version mismatch", s->p.ringName);
        goto out;
    }
    // HomeKit live view is H.264 only and we don't transcode: an H.265 ring would go out as broken "H.264".
    // 0 = RSS_CODEC_H264 (raptor_hal.h). Ending the stream makes iOS show the camera as unavailable.
    if (rss_ring_get_header(ring)->codec != 0) {
        HAPLogError(&logObject, "video: ring %s codec is %u, not H.264 (0); set codec = h264 for that stream in "
                    "raptor.conf for HomeKit live view. Stream not started.", s->p.ringName,
                    (unsigned) rss_ring_get_header(ring)->codec);
        goto out;
    }
    uint32_t maxFrame = rss_ring_max_frame_size(ring);
    frame = malloc(maxFrame);
    if (!frame)
        goto out;
    rss_ring_acquire(ring);
    rss_ring_request_idr(ring);

    const rss_ring_header_t* hdr = rss_ring_get_header(ring);
    uint64_t readSeq = atomic_load(&hdr->write_seq) + 1;
    bool gotKey = false;
    uint64_t start = now_ms(), lastRx = start, lastSr = 0, lastIdrReq = start, lastRrLog = 0;
    uint32_t frames = 0;
    HAPLogInfo(&logObject, "streaming %s (%ux%u requested) pt=%u mtu=%u", s->p.ringName, s->p.width, s->p.height,
               s->p.payloadType, s->p.mtu);

    while (!atomic_load(&s->stop)) {
        // Drain anything the controller sent (RTCP RR); it's our liveness signal.
        if (drain_rtcp(s->p.videoSock, &s->srtp, "video", 90000, &lastRrLog))
            lastRx = now_ms();
        if (!s->audioThreadStarted) { // otherwise the audio thread drains its own socket
            uint8_t rx[1500];
            while (recv(s->p.audioSock, rx, sizeof rx, MSG_DONTWAIT) > 0)
                lastRx = now_ms();
        } else {
            uint64_t a = atomic_load(&s->audioLastRx);
            if (a > lastRx)
                lastRx = a;
        }

        uint64_t now = now_ms();
        if (now - lastRx > RTCP_TIMEOUT_MS) {
            HAPLogInfo(&logObject, "no RTCP from controller for %u ms, ending stream", RTCP_TIMEOUT_MS);
            break;
        }
        if (atomic_exchange(&s->wantIdr, false) || (!gotKey && now - lastIdrReq > 1000)) {
            rss_ring_request_idr(ring);
            lastIdrReq = now;
        }

        int w = rss_ring_wait(ring, 50);
        (void) w;
        for (;;) {
            uint32_t len = 0;
            rss_ring_slot_t meta;
            int ret = rss_ring_read(ring, &readSeq, frame, maxFrame, &len, &meta);
            if (ret == RSS_EOVERFLOW) {
                readSeq = atomic_load(&hdr->write_seq);
                gotKey = false;
                continue;
            }
            if (ret != 0)
                break;
            if (!gotKey) {
                if (!meta.is_key)
                    continue;
                gotKey = true;
                HAPLogInfo(&logObject, "first keyframe after %u ms (%u bytes)", (unsigned)(now_ms() - start), len);
            }
            send_frame(s, frame, len, meta.timestamp);
            frames++;
        }
        now = now_ms();
        if (gotKey && now - lastSr >= SR_INTERVAL_MS) {
            send_rtcp_sr(s);
            lastSr = now;
        }
    }
    HAPLogInfo(&logObject, "stream ended after %u frames, %u packets", frames, s->pktCount);
    rss_ring_release(ring);
out:
    free(frame);
    if (ring)
        rss_ring_close(ring);
    atomic_store(&s->stop, true); // also stops the audio thread
    atomic_store(&s->finished, true);
    return NULL;
}

int stream_ring_codec(const char* ringName) {
    rss_ring_t* r = rss_ring_open(ringName);
    if (!r)
        return -1;
    int codec = rss_ring_version_ok(r, NULL) ? (int) rss_ring_get_header(r)->codec : -1;
    rss_ring_close(r);
    return codec;
}

// Marks a socket's traffic class so Wi-Fi (WMM) queues it accordingly: DSCP EF -> voice (AC_VO) for audio,
// AF41 -> video (AC_VI). Without it, audio waits behind each burst of video packets in the same queue.
static void set_traffic_class(int fd, int family, int dscp, int prio) {
    if (fd < 0)
        return;
    int tos = dscp << 2;
    if (family == AF_INET6)
        setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof tos);
    else
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof prio);
}

stream_t* stream_start(const stream_params_t* p) {
    stream_t* s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->p = *p;
    set_traffic_class(s->p.videoSock, s->p.videoDst.ss_family, 34, 5); // AF41, video
    set_traffic_class(s->p.audioSock, s->p.videoDst.ss_family, 46, 6); // EF, voice
    if (s->p.mtu < 400 || s->p.mtu > 1500)
        s->p.mtu = 1378;
    if (srtp_init(&s->srtp, p->videoKey, p->videoSalt)) {
        free(s);
        return NULL;
    }
    uint32_t r[2];
    FILE* f = fopen("/dev/urandom", "r");
    if (f) {
        if (fread(r, sizeof r, 1, f) != 1)
            r[0] = r[1] = (uint32_t) time(NULL);
        fclose(f);
    }
    s->seq = (uint16_t) r[0];
    s->tsOffset = r[1];
    s->audioSeq = (uint16_t)(r[0] >> 16);
    s->audioTsOffset = r[1] * 2654435761u;
    if (pthread_create(&s->thread, NULL, stream_thread, s)) {
        srtp_free(&s->srtp);
        free(s);
        return NULL;
    }
    if (s->p.audioEnabled && s->p.audioRingName && srtp_init(&s->audioSrtp, p->audioKey, p->audioSalt) == 0) {
        s->audioSrtpInit = true;
        if (pthread_create(&s->audioThread, NULL, audio_thread, s) == 0)
            s->audioThreadStarted = true;
    }
    return s;
}

void stream_stop(stream_t* s) {
    if (!s)
        return;
    atomic_store(&s->stop, true);
    pthread_join(s->thread, NULL);
    if (s->audioThreadStarted)
        pthread_join(s->audioThread, NULL);
    if (s->audioSrtpInit)
        srtp_free(&s->audioSrtp);
    srtp_free(&s->srtp);
    if (s->p.videoSock >= 0)
        close(s->p.videoSock);
    if (s->p.audioSock >= 0)
        close(s->p.audioSock);
    free(s);
}

bool stream_finished(const stream_t* s) {
    return s && atomic_load(&s->finished);
}

void stream_set_audio_muted(stream_t* s, bool muted) {
    if (s)
        atomic_store(&s->audioMuted, muted);
}

void stream_request_idr(stream_t* s) {
    if (s)
        atomic_store(&s->wantIdr, true);
}
