// SPDX-License-Identifier: GPL-3.0-or-later
// Opus packets (raptor's audio ring) -> 16-bit PCM (libopus) -> raw AAC-LC frames (faac), for HKSV recordings.
#ifndef RHKD_AACPIPE_H
#define RHKD_AACPIPE_H

#include <stddef.h>
#include <stdint.h>

typedef struct aacpipe aacpipe_t;

// Called for each finished AAC frame (1024 samples). @tsUs is the capture time of its first sample.
typedef void (*aacpipe_out_fn)(void* ctx, const uint8_t* aac, size_t len, int64_t tsUs);

// Mono AAC-LC at @sampleRate (Hz) and @bitrate (bit/s). NULL on failure.
aacpipe_t* aacpipe_open(uint32_t sampleRate, uint32_t bitrate);
void aacpipe_close(aacpipe_t* p);

// AudioSpecificConfig for the esds box.
const uint8_t* aacpipe_asc(aacpipe_t* p, size_t* len);

// Decodes one Opus packet captured at @tsUs into the PCM buffer (any thread). Returns the samples waiting, or -1.
int aacpipe_decode(aacpipe_t* p, const uint8_t* opus, size_t len, int64_t tsUs);

// Encodes every complete 1024-sample block (any one thread). Returns the frames emitted.
int aacpipe_encode(aacpipe_t* p, aacpipe_out_fn out, void* ctx);

#define AACPIPE_FRAME_SAMPLES 1024

#endif
