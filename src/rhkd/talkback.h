// Two-way audio: the controller's Opus (from Home's talk button) -> PCM -> rad's "speaker" ring -> camera speaker.
// SPDX-License-Identifier: GPL-3.0-or-later (links raptor-ipc)
#ifndef RHKD_TALKBACK_H
#define RHKD_TALKBACK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct talkback talkback_t;

// One per live-view session. @payloadType: the negotiated audio payload type (packets with another type are ignored).
talkback_t* talkback_open(uint8_t payloadType);
// Feeds one decrypted RTP packet (header + payload).
void talkback_rtp(talkback_t* t, const uint8_t* rtp, size_t len);
void talkback_close(talkback_t* t); // logs what was played; NULL is fine

// Speaker service state (Home's Mute and Volume), shared by all sessions.
void talkback_set_muted(bool muted);
bool talkback_muted(void);
void talkback_set_volume(uint8_t percent); // 0..100, 100 = as decoded
uint8_t talkback_volume(void);

// Echo cancellation switch: when on, rad's AEC runs while talk-back plays (see talkback.c).
void talkback_set_echo_cancel(bool on);
bool talkback_echo_cancel(void);
void talkback_shutdown(void); // turns off an AEC rhkd turned on
#endif
