// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal fragmented MP4 writer (see fmp4.h). Box layouts follow ISO/IEC 14496-12 and 14496-15 (avcC).
#include "fmp4.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------------
// Buffer helpers

void fmp4_buf_free(fmp4_buf_t* o) {
    free(o->b);
    o->b = NULL;
    o->len = o->cap = 0;
}

static void need(fmp4_buf_t* o, size_t n) {
    if (o->failed || o->len + n <= o->cap)
        return;
    size_t cap = o->cap ? o->cap : 4096;
    while (cap < o->len + n)
        cap *= 2;
    uint8_t* b = realloc(o->b, cap);
    if (!b) {
        o->failed = true;
        return;
    }
    o->b = b;
    o->cap = cap;
}

static void put(fmp4_buf_t* o, const void* p, size_t n) {
    need(o, n);
    if (o->failed)
        return;
    memcpy(o->b + o->len, p, n);
    o->len += n;
}

static void u8(fmp4_buf_t* o, uint8_t v) {
    put(o, &v, 1);
}

static void u16(fmp4_buf_t* o, uint16_t v) {
    uint8_t b[2] = { (uint8_t) (v >> 8), (uint8_t) v };
    put(o, b, 2);
}

static void u32(fmp4_buf_t* o, uint32_t v) {
    uint8_t b[4] = { (uint8_t) (v >> 24), (uint8_t) (v >> 16), (uint8_t) (v >> 8), (uint8_t) v };
    put(o, b, 4);
}

static void u64(fmp4_buf_t* o, uint64_t v) {
    u32(o, (uint32_t) (v >> 32));
    u32(o, (uint32_t) v);
}

static void zeros(fmp4_buf_t* o, size_t n) {
    need(o, n);
    if (o->failed)
        return;
    memset(o->b + o->len, 0, n);
    o->len += n;
}

// Opens a box: writes a size placeholder and the type, returns the offset to patch.
static size_t box(fmp4_buf_t* o, const char* type) {
    size_t at = o->len;
    u32(o, 0);
    put(o, type, 4);
    return at;
}

static size_t fullbox(fmp4_buf_t* o, const char* type, uint8_t version, uint32_t flags) {
    size_t at = box(o, type);
    u32(o, ((uint32_t) version << 24) | (flags & 0xFFFFFF));
    return at;
}

static void end(fmp4_buf_t* o, size_t at) {
    if (o->failed)
        return;
    uint32_t n = (uint32_t) (o->len - at);
    o->b[at] = (uint8_t) (n >> 24);
    o->b[at + 1] = (uint8_t) (n >> 16);
    o->b[at + 2] = (uint8_t) (n >> 8);
    o->b[at + 3] = (uint8_t) n;
}

static void matrix(fmp4_buf_t* o) {
    static const uint32_t m[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
    for (int i = 0; i < 9; i++)
        u32(o, m[i]);
}

// ---------------------------------------------------------------------------------------------------------------------
// Annex B

typedef struct {
    const uint8_t* p;
    size_t n;
} nal_t;

// Splits an Annex B buffer into NAL units (without start codes). Returns the count.
static size_t split(const uint8_t* d, size_t len, nal_t* out, size_t max) {
    size_t count = 0, i = 0, start = (size_t) -1;
    while (i + 3 <= len) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (i + 4 <= len && d[i + 2] == 0 && d[i + 3] == 1))) {
            size_t sc = d[i + 2] == 1 ? 3 : 4;
            if (start != (size_t) -1 && count < max) {
                size_t e = i;
                while (e > start && d[e - 1] == 0)
                    e--;
                out[count++] = (nal_t) { d + start, e - start };
            }
            i += sc;
            start = i;
        } else {
            i++;
        }
    }
    if (start != (size_t) -1 && start < len && count < max)
        out[count++] = (nal_t) { d + start, len - start };
    return count;
}

bool fmp4_find_params(const uint8_t* au, size_t len, const uint8_t** sps, size_t* spsLen, const uint8_t** pps,
                      size_t* ppsLen) {
    nal_t n[32];
    size_t c = split(au, len, n, 32);
    *sps = *pps = NULL;
    for (size_t i = 0; i < c; i++) {
        uint8_t t = n[i].p[0] & 0x1F;
        if (t == 7 && !*sps) {
            *sps = n[i].p;
            *spsLen = n[i].n;
        } else if (t == 8 && !*pps) {
            *pps = n[i].p;
            *ppsLen = n[i].n;
        }
    }
    return *sps && *pps && *spsLen >= 4;
}

// ---------------------------------------------------------------------------------------------------------------------
// Init segment

static void audio_trak(fmp4_buf_t* o, const uint8_t* asc, size_t ascLen, uint32_t rate) {
    size_t trak = box(o, "trak");
    size_t b = fullbox(o, "tkhd", 0, 0x000003);
    u32(o, 0);
    u32(o, 0);
    u32(o, 2); // track_ID
    u32(o, 0);
    u32(o, 0);
    zeros(o, 8);
    u16(o, 0);
    u16(o, 1);      // alternate_group
    u16(o, 0x0100); // volume
    u16(o, 0);
    matrix(o);
    u32(o, 0);
    u32(o, 0);
    end(o, b);
    size_t mdia = box(o, "mdia");
    b = fullbox(o, "mdhd", 0, 0);
    u32(o, 0);
    u32(o, 0);
    u32(o, rate);
    u32(o, 0);
    u16(o, 0x55C4);
    u16(o, 0);
    end(o, b);
    b = fullbox(o, "hdlr", 0, 0);
    u32(o, 0);
    put(o, "soun", 4);
    zeros(o, 12);
    put(o, "SoundHandler", 13);
    end(o, b);
    size_t minf = box(o, "minf");
    b = fullbox(o, "smhd", 0, 0);
    u32(o, 0);
    end(o, b);
    size_t dinf = box(o, "dinf");
    size_t dref = fullbox(o, "dref", 0, 0);
    u32(o, 1);
    b = fullbox(o, "url ", 0, 1);
    end(o, b);
    end(o, dref);
    end(o, dinf);
    size_t stbl = box(o, "stbl");
    size_t stsd = fullbox(o, "stsd", 0, 0);
    u32(o, 1);
    size_t mp4a = box(o, "mp4a");
    zeros(o, 6);
    u16(o, 1); // data_reference_index
    zeros(o, 8);
    u16(o, 1);  // channelcount
    u16(o, 16); // samplesize
    u16(o, 0);
    u16(o, 0);
    u32(o, rate << 16);
    size_t esds = fullbox(o, "esds", 0, 0);
    // ES_Descriptor { ES_ID 2, DecoderConfigDescriptor { mp4a, audio, DecoderSpecificInfo = ASC }, SLConfig }
    u8(o, 0x03);
    u8(o, (uint8_t) (3 + 2 + 13 + 2 + ascLen + 3));
    u16(o, 2);
    u8(o, 0);
    u8(o, 0x04);
    u8(o, (uint8_t) (13 + 2 + ascLen));
    u8(o, 0x40); // MPEG-4 audio
    u8(o, 0x15); // audio stream
    u8(o, 0);
    u16(o, 0); // bufferSizeDB
    u32(o, 0); // maxBitrate
    u32(o, 0); // avgBitrate
    u8(o, 0x05);
    u8(o, (uint8_t) ascLen);
    put(o, asc, ascLen);
    u8(o, 0x06);
    u8(o, 1);
    u8(o, 2);
    end(o, esds);
    end(o, mp4a);
    end(o, stsd);
    b = fullbox(o, "stts", 0, 0);
    u32(o, 0);
    end(o, b);
    b = fullbox(o, "stsc", 0, 0);
    u32(o, 0);
    end(o, b);
    b = fullbox(o, "stsz", 0, 0);
    u32(o, 0);
    u32(o, 0);
    end(o, b);
    b = fullbox(o, "stco", 0, 0);
    u32(o, 0);
    end(o, b);
    end(o, stbl);
    end(o, minf);
    end(o, mdia);
    end(o, trak);
}

bool fmp4_init_segment(fmp4_buf_t* o, uint16_t width, uint16_t height, const uint8_t* sps, size_t spsLen,
                       const uint8_t* pps, size_t ppsLen, const uint8_t* asc, size_t ascLen, uint32_t audioRate) {
    bool audio = asc && ascLen && ascLen < 64 && audioRate;
    size_t b = box(o, "ftyp");
    put(o, "iso5", 4);
    u32(o, 512);
    put(o, "iso5iso6mp41", 12);
    end(o, b);

    size_t moov = box(o, "moov");
    b = fullbox(o, "mvhd", 0, 0);
    u32(o, 0);    // creation
    u32(o, 0);    // modification
    u32(o, 1000); // timescale
    u32(o, 0);    // duration (fragmented)
    u32(o, 0x00010000);
    u16(o, 0x0100);
    zeros(o, 10);
    matrix(o);
    zeros(o, 24); // pre_defined
    u32(o, audio ? 3 : 2); // next_track_ID
    end(o, b);

    size_t trak = box(o, "trak");
    b = fullbox(o, "tkhd", 0, 0x000003); // enabled, in movie
    u32(o, 0);
    u32(o, 0);
    u32(o, 1); // track_ID
    u32(o, 0);
    u32(o, 0); // duration
    zeros(o, 8);
    u16(o, 0); // layer
    u16(o, 0); // alternate_group
    u16(o, 0); // volume (video)
    u16(o, 0);
    matrix(o);
    u32(o, (uint32_t) width << 16);
    u32(o, (uint32_t) height << 16);
    end(o, b);

    size_t mdia = box(o, "mdia");
    b = fullbox(o, "mdhd", 0, 0);
    u32(o, 0);
    u32(o, 0);
    u32(o, FMP4_TIMESCALE);
    u32(o, 0);
    u16(o, 0x55C4); // "und"
    u16(o, 0);
    end(o, b);
    b = fullbox(o, "hdlr", 0, 0);
    u32(o, 0);
    put(o, "vide", 4);
    zeros(o, 12);
    put(o, "VideoHandler", 13); // with NUL
    end(o, b);

    size_t minf = box(o, "minf");
    b = fullbox(o, "vmhd", 0, 1);
    zeros(o, 8);
    end(o, b);
    size_t dinf = box(o, "dinf");
    size_t dref = fullbox(o, "dref", 0, 0);
    u32(o, 1);
    b = fullbox(o, "url ", 0, 1); // self-contained
    end(o, b);
    end(o, dref);
    end(o, dinf);

    size_t stbl = box(o, "stbl");
    size_t stsd = fullbox(o, "stsd", 0, 0);
    u32(o, 1);
    size_t avc1 = box(o, "avc1");
    zeros(o, 6);
    u16(o, 1); // data_reference_index
    zeros(o, 16);
    u16(o, width);
    u16(o, height);
    u32(o, 0x00480000); // 72 dpi
    u32(o, 0x00480000);
    u32(o, 0);
    u16(o, 1); // frame_count
    zeros(o, 32); // compressorname
    u16(o, 0x0018);
    u16(o, 0xFFFF);
    size_t avcC = box(o, "avcC");
    u8(o, 1);
    u8(o, sps[1]); // profile
    u8(o, sps[2]); // compatibility
    u8(o, sps[3]); // level
    u8(o, 0xFF);   // 4-byte NAL lengths
    u8(o, 0xE1);   // 1 SPS
    u16(o, (uint16_t) spsLen);
    put(o, sps, spsLen);
    u8(o, 1); // 1 PPS
    u16(o, (uint16_t) ppsLen);
    put(o, pps, ppsLen);
    end(o, avcC);
    end(o, avc1);
    end(o, stsd);
    b = fullbox(o, "stts", 0, 0);
    u32(o, 0);
    end(o, b);
    b = fullbox(o, "stsc", 0, 0);
    u32(o, 0);
    end(o, b);
    b = fullbox(o, "stsz", 0, 0);
    u32(o, 0);
    u32(o, 0);
    end(o, b);
    b = fullbox(o, "stco", 0, 0);
    u32(o, 0);
    end(o, b);
    end(o, stbl);
    end(o, minf);
    end(o, mdia);
    end(o, trak);
    if (audio)
        audio_trak(o, asc, ascLen, audioRate);

    size_t mvex = box(o, "mvex");
    for (uint32_t t = 1; t <= (audio ? 2u : 1u); t++) {
        b = fullbox(o, "trex", 0, 0);
        u32(o, t); // track_ID
        u32(o, 1); // default_sample_description_index
        u32(o, 0);
        u32(o, 0);
        u32(o, 0);
        end(o, b);
    }
    end(o, mvex);
    end(o, moov);
    return !o->failed;
}

// ---------------------------------------------------------------------------------------------------------------------
// Fragments

// Size of a sample once converted to AVCC (4-byte lengths), without AUD NALs.
static size_t avcc_size(const fmp4_sample_t* s) {
    nal_t n[64];
    size_t c = split(s->data, s->len, n, 64), total = 0;
    for (size_t i = 0; i < c; i++)
        if (n[i].n && (n[i].p[0] & 0x1F) != 9)
            total += 4 + n[i].n;
    return total;
}

static void put_avcc(fmp4_buf_t* o, const fmp4_sample_t* s) {
    nal_t n[64];
    size_t c = split(s->data, s->len, n, 64);
    for (size_t i = 0; i < c; i++)
        if (n[i].n && (n[i].p[0] & 0x1F) != 9) {
            u32(o, (uint32_t) n[i].n);
            put(o, n[i].p, n[i].n);
        }
}

static void patch32(fmp4_buf_t* o, size_t at, uint32_t v) {
    if (o->failed)
        return;
    o->b[at] = (uint8_t) (v >> 24);
    o->b[at + 1] = (uint8_t) (v >> 16);
    o->b[at + 2] = (uint8_t) (v >> 8);
    o->b[at + 3] = (uint8_t) v;
}

bool fmp4_fragment(fmp4_buf_t* o, uint32_t seq, uint64_t baseTime, const fmp4_sample_t* samples, size_t count,
                   const fmp4_audio_t* audio, size_t audioCount, uint64_t audioBase) {
    size_t* sizes = malloc(count * sizeof *sizes);
    if (!sizes)
        return false;
    for (size_t i = 0; i < count; i++)
        sizes[i] = avcc_size(&samples[i]);

    size_t moofAt = box(o, "moof");
    size_t b = fullbox(o, "mfhd", 0, 0);
    u32(o, seq);
    end(o, b);
    size_t traf = box(o, "traf");
    b = fullbox(o, "tfhd", 0, 0x020000); // default-base-is-moof
    u32(o, 1);
    end(o, b);
    b = fullbox(o, "tfdt", 1, 0);
    u64(o, baseTime);
    end(o, b);
    // data offset, per-sample duration, size and flags
    size_t trun = fullbox(o, "trun", 0, 0x000001 | 0x000100 | 0x000200 | 0x000400);
    u32(o, (uint32_t) count);
    size_t dataOffsetAt = o->len;
    u32(o, 0);
    for (size_t i = 0; i < count; i++) {
        u32(o, samples[i].duration);
        u32(o, (uint32_t) sizes[i]);
        // sync: depends_on = 2 (no other); non-sync: depends_on = 1, sample_is_non_sync_sample
        u32(o, samples[i].key ? 0x02000000 : 0x01010000);
    }
    end(o, trun);
    end(o, traf);

    size_t audioOffsetAt = 0, audioTotal = 0;
    if (audioCount) {
        size_t atraf = box(o, "traf");
        b = fullbox(o, "tfhd", 0, 0x020000);
        u32(o, 2);
        end(o, b);
        b = fullbox(o, "tfdt", 1, 0);
        u64(o, audioBase);
        end(o, b);
        size_t atrun = fullbox(o, "trun", 0, 0x000001 | 0x000100 | 0x000200);
        u32(o, (uint32_t) audioCount);
        audioOffsetAt = o->len;
        u32(o, 0);
        for (size_t i = 0; i < audioCount; i++) {
            u32(o, 1024);
            u32(o, (uint32_t) audio[i].len);
            audioTotal += audio[i].len;
        }
        end(o, atrun);
        end(o, atraf);
    }
    end(o, moofAt);

    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += sizes[i];
    free(sizes);
    // data_offset: from the start of moof to the sample bytes (after the 8-byte mdat header); audio follows video.
    size_t moofLen = o->len - moofAt;
    patch32(o, dataOffsetAt, (uint32_t) (moofLen + 8));
    if (audioCount)
        patch32(o, audioOffsetAt, (uint32_t) (moofLen + 8 + total));
    u32(o, (uint32_t) (8 + total + audioTotal));
    put(o, "mdat", 4);
    for (size_t i = 0; i < count; i++)
        put_avcc(o, &samples[i]);
    for (size_t i = 0; i < audioCount; i++)
        put(o, audio[i].data, audio[i].len);
    return !o->failed;
}
