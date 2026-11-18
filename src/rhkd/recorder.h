// SPDX-License-Identifier: GPL-3.0-or-later
// HomeKit Secure Video recorder: prebuffers the main H.264 ring while recording is active, and streams
// fMP4 (init segment + keyframe-aligned fragments) over HDS dataSend when a controller opens a recording.
#ifndef RHKD_RECORDER_H
#define RHKD_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#include "hds.h"

// Recording Active on/off (HAP run loop). On starts the prebuffer thread; off stops it (and any stream).
void recorder_set_active(bool on, uint32_t prebufferMs, uint32_t fragmentMs);

// RecordingAudioActive: include AAC-LC audio (from the Opus ring) at @bitrate bit/s.
void recorder_set_audio(bool on, uint32_t bitrate);

// True while a recording stream is open.
bool recorder_busy(void);

// Start streaming to @conn (after the dataSend/open success response). Returns false if it can't.
bool recorder_start(hds_conn_t* conn, int64_t streamId);

// Stop streaming to @conn (controller close/ack, connection closed). Optionally sends dataSend/close.
void recorder_stop(hds_conn_t* conn, bool sendClose, int reason);

#endif
