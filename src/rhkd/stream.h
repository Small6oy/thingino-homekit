// RTP/SRTP H.264 (+ Opus audio) sender fed from raptor rings.
// SPDX-License-Identifier: GPL-3.0-or-later (links raptor-ipc)
#ifndef RHKD_STREAM_H
#define RHKD_STREAM_H
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct {
    // From SetupEndpoints
    struct sockaddr_storage videoDst; // controller address + video port
    socklen_t videoDstLen;
    int videoSock;                    // bound local UDP socket (RTP + RTCP mux)
    int audioSock;
    uint8_t videoKey[16], videoSalt[14];
    uint32_t videoSSRC;               // our SSRC
    // From SelectedRTPStreamConfiguration
    uint8_t payloadType;
    uint16_t width, height, fps;
    uint16_t maxBitrateKbps;
    uint16_t mtu;
    const char* ringName;             // "main" or "sub"

    // Audio (camera -> controller). Opus from rad's "audio" ring.
    bool audioEnabled;
    struct sockaddr_storage audioDst; // controller address + audio port
    socklen_t audioDstLen;
    uint8_t audioKey[16], audioSalt[14];
    uint32_t audioSSRC;               // our SSRC
    uint8_t audioPayloadType;
    uint8_t audioPacketMs;            // negotiated packet time (rtp time), ms
    uint32_t audioClockHz;            // RTP clock for audio timestamps (negotiated rate, or 48000)
    const char* audioRingName;        // "audio"
} stream_params_t;

typedef struct stream stream_t;

stream_t* stream_start(const stream_params_t* p);
void stream_stop(stream_t* s);           // joins the thread and closes sockets
bool stream_finished(const stream_t* s); // true if the thread ended on its own (timeout/error)
void stream_request_idr(stream_t* s);
void stream_set_audio_muted(stream_t* s, bool muted); // muted: audio packets are not sent
int stream_ring_codec(const char* ringName);          // rss codec of a ring (0 = H.264, 111 = Opus), -1 if unavailable
#endif
