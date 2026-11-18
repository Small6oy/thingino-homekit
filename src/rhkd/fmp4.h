// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal fragmented MP4 (ISO BMFF) writer for one H.264 video track, for HomeKit Secure Video.
// Layout as ffmpeg's -movflags frag_keyframe+empty_moov+default_base_moof: an init segment (ftyp + moov with
// mvex) and fragments of moof (mfhd, traf: tfhd default-base-is-moof, tfdt, trun) + mdat.
#ifndef RHKD_FMP4_H
#define RHKD_FMP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FMP4_TIMESCALE 90000

typedef struct {
    const uint8_t* data; // Annex B access unit (start codes), as raptor publishes it
    size_t len;
    uint32_t duration;   // in FMP4_TIMESCALE units
    bool key;
} fmp4_sample_t;

typedef struct {
    const uint8_t* data; // one raw AAC frame (1024 samples)
    size_t len;
} fmp4_audio_t;

// Growable output buffer.
typedef struct {
    uint8_t* b;
    size_t len, cap;
    bool failed;
} fmp4_buf_t;

void fmp4_buf_free(fmp4_buf_t* o);

// Finds SPS and PPS in an Annex B keyframe. Returns false if either is missing.
bool fmp4_find_params(const uint8_t* au, size_t len, const uint8_t** sps, size_t* spsLen, const uint8_t** pps,
                      size_t* ppsLen);

// Init segment: track 1 = H.264 (width x height); track 2 = mono AAC at @audioRate if @asc is not NULL.
bool fmp4_init_segment(fmp4_buf_t* o, uint16_t width, uint16_t height, const uint8_t* sps, size_t spsLen,
                       const uint8_t* pps, size_t ppsLen, const uint8_t* asc, size_t ascLen, uint32_t audioRate);

// One fragment (moof + mdat). @seq starts at 1; @baseTime is the decode time of the first video sample (90 kHz);
// @audioBase that of the first AAC frame (in audioRate units). Pass audioCount 0 for none.
bool fmp4_fragment(fmp4_buf_t* o, uint32_t seq, uint64_t baseTime, const fmp4_sample_t* samples, size_t count,
                   const fmp4_audio_t* audio, size_t audioCount, uint64_t audioBase);

#endif
