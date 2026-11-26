// rhkd — HomeKit IP camera accessory for Thingino/raptor.
//
// Camera RTP stream management, ported from HAP-NodeJS
// src/lib/camera/RTPStreamManagement.ts (Apache-2.0, Copyright The HAP-NodeJS contributors).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "HAP.h"
#include "HAPPlatformTimer.h"
#include "HAPUUID.h"
#include "version.h"
#include "motion.h"
#include "opmode.h"
#include "hds.h"
#include "recording.h"
#include "rtpdump.h"
#include "rss_ipc.h"

#include "App.h"
#include "DB.h"
#include "bitrate.h"
#include "snapshot.h"
#include "stream.h"
#include "talkback.h"
#include "tlvb.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "Camera" };

// ---------------------------------------------------------------------------------------------------------------------
// HAP types not defined by the open-source ADK.

static const HAPUUID kCharType_SupportedVideoStreamConfiguration = HAPUUIDCreateAppleDefined(0x114);
static const HAPUUID kCharType_SupportedAudioStreamConfiguration = HAPUUIDCreateAppleDefined(0x115);
static const HAPUUID kCharType_SupportedRTPConfiguration = HAPUUIDCreateAppleDefined(0x116);
static const HAPUUID kCharType_SelectedRTPStreamConfiguration = HAPUUIDCreateAppleDefined(0x117);
static const HAPUUID kCharType_SetupEndpoints = HAPUUIDCreateAppleDefined(0x118);
static const HAPUUID kCharType_StreamingStatus = HAPUUIDCreateAppleDefined(0x120);
static const HAPUUID kCharType_Mute = HAPUUIDCreateAppleDefined(0x11A);

// TLV enums (names follow HAP-NodeJS RTPStreamManagement.ts).
enum { kStreamingStatus_Available = 0, kStreamingStatus_InUse = 1, kStreamingStatus_Unavailable = 2 };
enum { kSetupStatus_Success = 0, kSetupStatus_Busy = 1, kSetupStatus_Error = 2 };
enum { kCmd_End = 0, kCmd_Start = 1, kCmd_Suspend = 2, kCmd_Resume = 3, kCmd_Reconfigure = 4 };

// ---------------------------------------------------------------------------------------------------------------------
// State (only touched on the HAP run loop thread).

// One per Camera RTP Stream Management service: each carries one live view, so two services allow two viewers at once
// (as the G100 does). The first takes any size; the second is the extra viewer.
typedef struct {
    int index;
    const HAPService* service;
    const HAPCharacteristic* statusCharacteristic;
    const HAPCharacteristic* activeCharacteristic;
    uint8_t active;
    uint8_t streamingStatus;

    // SetupEndpoints
    bool haveSetup;
    uint8_t sessionId[16];
    stream_params_t pending;
    uint8_t setupResponse[512];
    size_t setupResponseLen;
    HAPSessionRef* setupSession;

    // SelectedRTPStreamConfiguration (echoed back on read)
    uint8_t selected[512];
    size_t selectedLen;

    stream_t* stream;
    HAPSessionRef* streamSession;
} rtp_slot_t;

#define kRtpSlots 2

static struct {
    HAPAccessoryServerRef* server;
    HAPPlatformKeyValueStoreRef keyValueStore;
    rtp_slot_t slot[kRtpSlots];
    HAPPlatformTimerRef watchdog;
} cam;

extern const HAPAccessory* AppGetAccessoryInfo(void);
static const HAPService cameraService;
static const HAPService cameraService2;
static const HAPTLV8Characteristic streamingStatusCharacteristic;
static const HAPTLV8Characteristic streamingStatusCharacteristic2;
static const HAPUInt8Characteristic activeCharacteristic;
static const HAPUInt8Characteristic activeCharacteristic2;

static rtp_slot_t* SlotFor(const HAPService* service) {
    for (int i = 0; i < kRtpSlots; i++)
        if (cam.slot[i].service && service && cam.slot[i].service->iid == service->iid)
            return &cam.slot[i];
    return &cam.slot[0];
}

static bool AnyStreaming(void) {
    for (int i = 0; i < kRtpSlots; i++)
        if (cam.slot[i].stream)
            return true;
    return false;
}

// Microphone mute (Microphone service). When muted, audio packets are not sent.
static bool micMuted = false;

// ---------------------------------------------------------------------------------------------------------------------
// Helpers

// Streaming is refused while the RTP service is inactive or the camera is turned off in Home (HomeKitCameraActive),
// as HAP-NodeJS RTPStreamManagement.streamingIsDisabled does.
static bool StreamingDisabled(const rtp_slot_t* s) {
    return !s->active || !opmode_homekit_active();
}

// Appends every top-level item of a raw TLV buffer to an ADK TLV writer.
static HAPError EmitRaw(HAPTLVWriterRef* w, const uint8_t* buf, size_t len) {
    static uint8_t val[1024];
    size_t pos = 0, n;
    uint8_t type;
    while (pos < len) {
        if (!tlv_next(buf, len, &pos, &type, val, sizeof val, &n))
            return kHAPError_Unknown;
        HAPError err = HAPTLVWriterAppend(w, &(const HAPTLV) { .type = type, .value = { .bytes = val, .numBytes = n } });
        if (err)
            return err;
    }
    return kHAPError_None;
}

static void SetStreamingStatus(rtp_slot_t* s, uint8_t status) {
    if (s->streamingStatus == status)
        return;
    s->streamingStatus = status;
    if (cam.server)
        HAPAccessoryServerRaiseEvent(cam.server, s->statusCharacteristic, s->service, AppGetAccessoryInfo());
}

static void CloseSetupSockets(rtp_slot_t* s) {
    if (s->pending.videoSock >= 0)
        close(s->pending.videoSock);
    if (s->pending.audioSock >= 0)
        close(s->pending.audioSock);
    s->pending.videoSock = s->pending.audioSock = -1;
}

static void EndSession(rtp_slot_t* s, const char* why) {
    if (s->stream) {
        HAPLogInfo(&logObject, "Ending stream %d (%s).", s->index, why);
        stream_stop(s->stream); // closes the sockets it owns
        s->stream = NULL;
    } else if (s->haveSetup) {
        CloseSetupSockets(s);
    }
    s->haveSetup = false;
    s->streamSession = NULL;
    s->setupSession = NULL;
    s->selectedLen = 0;
    bitrate_release(s->index);
    if (!AnyStreaming())
        opmode_stream_ended();
    SetStreamingStatus(s, kStreamingStatus_Available);
}

static void EndAllSessions(const char* why) {
    for (int i = 0; i < kRtpSlots; i++)
        EndSession(&cam.slot[i], why);
}

static int BindUdp(int family, uint16_t* port) {
    int fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    socklen_t sl;
    if (family == AF_INET) {
        ((struct sockaddr_in*) &ss)->sin_family = AF_INET;
        sl = sizeof(struct sockaddr_in);
    } else {
        ((struct sockaddr_in6*) &ss)->sin6_family = AF_INET6;
        sl = sizeof(struct sockaddr_in6);
    }
    if (bind(fd, (struct sockaddr*) &ss, sl) < 0) {
        close(fd);
        return -1;
    }
    int sndbuf = 512 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    sl = sizeof ss;
    getsockname(fd, (struct sockaddr*) &ss, &sl);
    *port = ntohs(family == AF_INET ? ((struct sockaddr_in*) &ss)->sin_port : ((struct sockaddr_in6*) &ss)->sin6_port);
    return fd;
}

// The accessory address the controller should send to: the local address used to reach it.
static bool LocalAddressFor(const struct sockaddr_storage* dst, socklen_t dl, char* out, size_t cap) {
    int fd = socket(dst->ss_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    bool ok = false;
    struct sockaddr_storage me;
    socklen_t ml = sizeof me;
    if (connect(fd, (const struct sockaddr*) dst, dl) == 0 && getsockname(fd, (struct sockaddr*) &me, &ml) == 0) {
        const void* a = dst->ss_family == AF_INET ? (const void*) &((struct sockaddr_in*) &me)->sin_addr :
                                                     (const void*) &((struct sockaddr_in6*) &me)->sin6_addr;
        ok = inet_ntop(dst->ss_family, a, out, (socklen_t) cap) != NULL;
    }
    close(fd);
    return ok;
}

static void RandomBytes(void* p, size_t n) {
    HAPPlatformRandomNumberFill(p, n);
}

// ---------------------------------------------------------------------------------------------------------------------
// Supported configurations

static const uint16_t kResolutions[][3] = {
    { 1920, 1080, 30 }, { 1280, 960, 30 }, { 1280, 720, 30 }, { 1024, 768, 30 }, { 640, 480, 30 }, { 640, 360, 30 },
    { 480, 360, 30 },   { 480, 270, 30 },  { 320, 240, 30 },  { 320, 240, 15 },  { 320, 180, 30 },
};

// maxWidth: the second RTP service offers up to 1280x720, like the certified Aqara G100 (its svc 2).
static size_t BuildSupportedVideo(uint8_t* out, size_t cap, unsigned maxWidth) {
    uint8_t params[64], attrs[512], cfg[640];
    tlvb_t b;

    tlvb_init(&b, params, sizeof params);
    for (uint8_t p = 0; p <= 2; p++) { // Baseline, Main, High
        if (p)
            tlvb_sep(&b);
        tlvb_u8(&b, 0x01, p);
    }
    for (uint8_t l = 0; l <= 2; l++) { // 3.1, 3.2, 4.0
        if (l)
            tlvb_sep(&b);
        tlvb_u8(&b, 0x02, l);
    }
    tlvb_u8(&b, 0x03, 0); // packetization mode: non-interleaved
    size_t paramsLen = b.len;

    tlvb_init(&b, attrs, sizeof attrs);
    bool first = true;
    for (size_t i = 0; i < HAPArrayCount(kResolutions); i++) {
        if (kResolutions[i][0] > maxWidth)
            continue;
        uint8_t a[16];
        tlvb_t ab;
        tlvb_init(&ab, a, sizeof a);
        tlvb_u16(&ab, 0x01, kResolutions[i][0]);
        tlvb_u16(&ab, 0x02, kResolutions[i][1]);
        tlvb_u8(&ab, 0x03, (uint8_t) kResolutions[i][2]);
        if (!first)
            tlvb_sep(&b);
        first = false;
        tlvb_put(&b, 0x03, a, ab.len);
    }
    size_t attrsLen = b.len;

    tlvb_init(&b, cfg, sizeof cfg);
    tlvb_u8(&b, 0x01, 0); // H.264
    tlvb_put(&b, 0x02, params, paramsLen);
    // The attribute list is already a sequence of 0x03 items with delimiters: append verbatim.
    if (b.len + attrsLen <= b.cap) {
        memcpy(b.buf + b.len, attrs, attrsLen);
        b.len += attrsLen;
    }
    size_t cfgLen = b.len;

    tlvb_init(&b, out, cap);
    tlvb_put(&b, 0x01, cfg, cfgLen);
    return b.overflow ? 0 : b.len;
}

static size_t BuildSupportedAudio(uint8_t* out, size_t cap) {
    uint8_t params[32], codec[64];
    tlvb_t b;
    tlvb_init(&b, params, sizeof params);
    tlvb_u8(&b, 0x01, 1); // channels
    tlvb_u8(&b, 0x02, 0); // variable bitrate
    tlvb_u8(&b, 0x03, 1); // 16 kHz (rad encodes Opus at 16 kHz)
    size_t pl = b.len;
    tlvb_init(&b, codec, sizeof codec);
    tlvb_u8(&b, 0x01, 3); // Opus
    tlvb_put(&b, 0x02, params, pl);
    size_t cl = b.len;
    tlvb_init(&b, out, cap);
    tlvb_put(&b, 0x01, codec, cl);
    tlvb_u8(&b, 0x02, 0); // no comfort noise
    return b.overflow ? 0 : b.len;
}

// ---------------------------------------------------------------------------------------------------------------------
// Characteristic handlers

#define kIID_Camera2SupportedVideo   ((uint64_t) 0x00A2) // also below, with the second service

static HAPError HandleSupportedVideoRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    uint8_t buf[1024];
    size_t n = BuildSupportedVideo(buf, sizeof buf,
                                   request->characteristic->iid == kIID_Camera2SupportedVideo ? 1280 : 1920);
    return n ? EmitRaw(w, buf, n) : kHAPError_Unknown;
}

static HAPError HandleSupportedAudioRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request HAP_UNUSED,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    uint8_t buf[128];
    size_t n = BuildSupportedAudio(buf, sizeof buf);
    return n ? EmitRaw(w, buf, n) : kHAPError_Unknown;
}

static HAPError HandleSupportedRTPRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request HAP_UNUSED,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    uint8_t suite = 0; // AES_CM_128_HMAC_SHA1_80
    return HAPTLVWriterAppend(w, &(const HAPTLV) { .type = 0x02, .value = { .bytes = &suite, .numBytes = 1 } });
}

static HAPError HandleStreamingStatusRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    rtp_slot_t* s = SlotFor(request->service);
    uint8_t st = s->active ? s->streamingStatus : kStreamingStatus_Unavailable;
    return HAPTLVWriterAppend(w, &(const HAPTLV) { .type = 0x01, .value = { .bytes = &st, .numBytes = 1 } });
}

static HAPError HandleSetupEndpointsRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    rtp_slot_t* s = SlotFor(request->service);
    if (StreamingDisabled(s)) {
        uint8_t status = kSetupStatus_Error; // as HAP-NodeJS while streaming is disabled
        return HAPTLVWriterAppend(w, &(const HAPTLV) { .type = 0x02, .value = { .bytes = &status, .numBytes = 1 } });
    }
    if (!s->setupResponseLen)
        return kHAPError_None; // empty until a controller sets up endpoints (as HAP-NodeJS does)
    return EmitRaw(w, s->setupResponse, s->setupResponseLen);
}

// Collects the raw bytes of a write into a flat TLV buffer (re-encoded so our parser can walk it).
static bool CollectWrite(HAPTLVReaderRef* r, uint8_t* out, size_t cap, size_t* len) {
    tlvb_t b;
    tlvb_init(&b, out, cap);
    for (;;) {
        bool found;
        HAPTLV tlv;
        if (HAPTLVReaderGetNext(r, &found, &tlv))
            return false;
        if (!found)
            break;
        tlvb_put(&b, tlv.type, tlv.value.bytes, tlv.value.numBytes);
    }
    *len = b.len;
    return !b.overflow;
}

static HAPError HandleSetupEndpointsWrite(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicWriteRequest* request,
        HAPTLVReaderRef* r,
        void* _Nullable context HAP_UNUSED) {
    rtp_slot_t* slot = SlotFor(request->service);
    if (StreamingDisabled(slot)) {
        HAPLogInfo(&logObject, "SetupEndpoints refused: streaming is disabled.");
        return kHAPError_InvalidState;
    }
    uint8_t req[512];
    size_t reqLen;
    if (!CollectWrite(r, req, sizeof req, &reqLen))
        return kHAPError_InvalidData;

    uint8_t sid[16], addr[128], vsrtp[64], asrtp[64];
    size_t sidLen, addrLen, vLen, aLen;
    if (!tlv_get(req, reqLen, 0x01, sid, sizeof sid, &sidLen) || sidLen != 16 ||
        !tlv_get(req, reqLen, 0x03, addr, sizeof addr, &addrLen) ||
        !tlv_get(req, reqLen, 0x04, vsrtp, sizeof vsrtp, &vLen) ||
        !tlv_get(req, reqLen, 0x05, asrtp, sizeof asrtp, &aLen)) {
        HAPLogError(&logObject, "SetupEndpoints: malformed request.");
        return kHAPError_InvalidData;
    }

    // A stream that died on its own (timeout) frees the slot.
    if (slot->stream && stream_finished(slot->stream))
        EndSession(slot, "finished");

    tlvb_t b;
    if (slot->stream) {
        HAPLogInfo(&logObject, "SetupEndpoints %d: busy.", slot->index);
        tlvb_init(&b, slot->setupResponse, sizeof slot->setupResponse);
        tlvb_put(&b, 0x01, sid, 16);
        tlvb_u8(&b, 0x02, kSetupStatus_Busy);
        slot->setupResponseLen = b.len;
        return kHAPError_None;
    }
    if (slot->haveSetup) // a previous setup that never started
        CloseSetupSockets(slot);

    uint8_t ver = 0;
    uint16_t vport = 0, aport = 0;
    char ip[64] = { 0 };
    size_t ipLen = 0;
    tlv_get_u8(addr, addrLen, 0x01, &ver);
    tlv_get(addr, addrLen, 0x02, ip, sizeof ip - 1, &ipLen);
    tlv_get_u16(addr, addrLen, 0x03, &vport);
    tlv_get_u16(addr, addrLen, 0x04, &aport);
    ip[ipLen] = '\0';

    stream_params_t* p = &slot->pending;
    memset(p, 0, sizeof *p);
    p->videoSock = p->audioSock = -1;
    int family = ver == 1 ? AF_INET6 : AF_INET;
    if (family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*) &p->videoDst;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(vport);
        if (inet_pton(AF_INET, ip, &sin->sin_addr) != 1)
            return kHAPError_InvalidData;
        p->videoDstLen = sizeof *sin;
    } else {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*) &p->videoDst;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(vport);
        if (inet_pton(AF_INET6, ip, &sin6->sin6_addr) != 1)
            return kHAPError_InvalidData;
        p->videoDstLen = sizeof *sin6;
    }
    memcpy(&p->audioDst, &p->videoDst, sizeof p->audioDst);
    p->audioDstLen = p->videoDstLen;
    if (family == AF_INET)
        ((struct sockaddr_in*) &p->audioDst)->sin_port = htons(aport);
    else
        ((struct sockaddr_in6*) &p->audioDst)->sin6_port = htons(aport);

    size_t kl = 0, sl = 0;
    uint8_t suite = 0;
    tlv_get_u8(vsrtp, vLen, 0x01, &suite);
    if (suite != 0 || !tlv_get(vsrtp, vLen, 0x02, p->videoKey, 16, &kl) || kl != 16 ||
        !tlv_get(vsrtp, vLen, 0x03, p->videoSalt, 14, &sl) || sl != 14) {
        HAPLogError(&logObject, "SetupEndpoints: unsupported SRTP suite %u.", suite);
        return kHAPError_InvalidData;
    }

    uint16_t myVport = 0, myAport = 0;
    p->videoSock = BindUdp(family, &myVport);
    p->audioSock = BindUdp(family, &myAport);
    char myIp[64];
    if (p->videoSock < 0 || p->audioSock < 0 || !LocalAddressFor(&p->videoDst, p->videoDstLen, myIp, sizeof myIp)) {
        CloseSetupSockets(slot);
        return kHAPError_Unknown;
    }
    RandomBytes(&p->videoSSRC, 4);
    RandomBytes(&p->audioSSRC, 4);

    uint8_t aaddr[96], vs[64], as[64];
    tlvb_t t;
    tlvb_init(&t, aaddr, sizeof aaddr);
    tlvb_u8(&t, 0x01, ver);
    tlvb_put(&t, 0x02, myIp, strlen(myIp));
    tlvb_u16(&t, 0x03, myVport);
    tlvb_u16(&t, 0x04, myAport);
    size_t aaddrLen = t.len;
    // Echo the controller's keys for both directions (HAP-NodeJS default behaviour).
    tlvb_init(&t, vs, sizeof vs);
    tlvb_u8(&t, 0x01, 0);
    tlvb_put(&t, 0x02, p->videoKey, 16);
    tlvb_put(&t, 0x03, p->videoSalt, 14);
    size_t vsLen = t.len;
    uint8_t ak[16] = { 0 }, as_[14] = { 0 };
    tlv_get(asrtp, aLen, 0x02, ak, 16, NULL);
    tlv_get(asrtp, aLen, 0x03, as_, 14, NULL);
    memcpy(p->audioKey, ak, 16);
    memcpy(p->audioSalt, as_, 14);
    tlvb_init(&t, as, sizeof as);
    tlvb_u8(&t, 0x01, 0);
    tlvb_put(&t, 0x02, ak, 16);
    tlvb_put(&t, 0x03, as_, 14);
    size_t asLen = t.len;

    tlvb_init(&b, slot->setupResponse, sizeof slot->setupResponse);
    tlvb_put(&b, 0x01, sid, 16);
    tlvb_u8(&b, 0x02, kSetupStatus_Success);
    tlvb_put(&b, 0x03, aaddr, aaddrLen);
    tlvb_put(&b, 0x04, vs, vsLen);
    tlvb_put(&b, 0x05, as, asLen);
    tlvb_u32(&b, 0x06, p->videoSSRC);
    tlvb_u32(&b, 0x07, p->audioSSRC);
    slot->setupResponseLen = b.len;

    memcpy(slot->sessionId, sid, 16);
    slot->haveSetup = true;
    slot->setupSession = request->session;
    HAPLogInfo(&logObject, "SetupEndpoints %d: controller %s:%u (video) -> us %s:%u.", slot->index, ip, vport, myIp, myVport);
    SetStreamingStatus(slot, kStreamingStatus_InUse);
    return kHAPError_None;
}

// The rad ring carrying Opus for live view. With rad's dual-codec output the main "audio" ring can be AAC (stock
// consumers, HKSV) and Opus sits in "audio2", encoded on demand while a reader holds it; otherwise "audio" is Opus.
// Falls back to "audio", where the stream thread reports a wrong codec instead of sending it.
static const char* OpusRingName(void) {
    static const char* const names[] = { "audio2", "audio3", "audio" };
    for (unsigned i = 0; i < 3; i++) {
        rss_ring_t* ring = rss_ring_open(names[i]);
        if (!ring)
            continue;
        bool opus = rss_ring_version_ok(ring, NULL) && rss_ring_get_header(ring)->codec == 111;
        rss_ring_close(ring);
        if (opus)
            return names[i];
    }
    return "audio";
}

static bool RingTaken(const rtp_slot_t* self, const char* ring) {
    for (int i = 0; i < kRtpSlots; i++) {
        const rtp_slot_t* o = &cam.slot[i];
        if (o != self && o->stream && o->pending.ringName && strcmp(o->pending.ringName, ring) == 0)
            return true;
    }
    return false;
}

// Live-view source. iOS states its budget as size + max bitrate; on the LAN it asks for 1280x720 at ~300 kbit/s.
// - aux (rvd [stream2], on demand): a per-session encoder at its configured size, whose bitrate follows this session
//   (bitrate.c). Used when no other session holds it and the request fits it -- or iOS asks for < 1 Mbit/s, where a
//   smaller picture at the asked bitrate beats main at its fixed one.
// - sub (640x360): small requests, or a tight budget when aux is absent or taken; its bitrate follows the session.
// - main: the full picture at its configured bitrate (shared with recordings, never changed).
// RHKD_VIDEO_RING=main|sub|aux forces one. HomeKit takes H.264 only, so a non-H.264 choice falls back.
static const char* ChooseVideoRing(const rtp_slot_t* slot, const stream_params_t* p) {
    static const char* const kRings[] = { "main", "sub", "aux" };
    const char* force = getenv("RHKD_VIDEO_RING");
    for (unsigned i = 0; force && i < HAPArrayCount(kRings); i++)
        if (strcmp(force, kRings[i]) == 0)
            return kRings[i];
    bool lowBudget = p->maxBitrateKbps && p->maxBitrateKbps < 1000;
    stream_ring_info_t aux;
    if (p->width > 640 && stream_ring_info("aux", &aux) && aux.codec == 0 && !RingTaken(slot, "aux") &&
        (lowBudget || (p->width <= aux.width && p->height <= aux.height)))
        return "aux";
    const char* ring = lowBudget || p->width <= 640 ? "sub" : "main";
    // main is never slowed down (recordings, Frigate), so only use it when iOS's budget covers its configured rate;
    // a certified camera stays at the bitrate iOS asks for (aqara-g100.md, HAP-NodeJS #959).
    if (strcmp(ring, "main") == 0 && p->maxBitrateKbps && stream_ring_codec("sub") == 0) {
        unsigned mainKbps = bitrate_configured_kbps("main");
        if (mainKbps && p->maxBitrateKbps < mainKbps) {
            HAPLogInfo(&logObject, "main runs at %u kbit/s, above the %u iOS allows; using sub.", mainKbps,
                       p->maxBitrateKbps);
            ring = "sub";
        }
    }
    if (stream_ring_codec(ring) != 0) {
        const char* other = strcmp(ring, "main") == 0 ? "sub" : "main";
        if (stream_ring_codec(other) == 0) {
            HAPLogInfo(&logObject, "Ring %s is not H.264; using %s instead.", ring, other);
            ring = other;
        }
    }
    return ring;
}

static HAPError HandleSelectedRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    rtp_slot_t* slot = SlotFor(request->service);
    if (StreamingDisabled(slot)) {
        // HAP-NodeJS answers {01 session control {02 command = suspend}} while streaming is disabled.
        const uint8_t suspend[] = { 0x02, 0x01, kCmd_Suspend };
        return HAPTLVWriterAppend(w, &(const HAPTLV) { .type = 0x01, .value = { .bytes = suspend, .numBytes = sizeof suspend } });
    }
    if (!slot->selectedLen)
        return kHAPError_None; // empty until a stream is selected (as HAP-NodeJS does)
    return EmitRaw(w, slot->selected, slot->selectedLen);
}

static HAPError HandleSelectedWrite(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicWriteRequest* request,
        HAPTLVReaderRef* r,
        void* _Nullable context HAP_UNUSED) {
    rtp_slot_t* slot = SlotFor(request->service);
    uint8_t req[512];
    size_t reqLen;
    if (!CollectWrite(r, req, sizeof req, &reqLen))
        return kHAPError_InvalidData;

    uint8_t ctl[64];
    size_t ctlLen;
    uint8_t cmd = 0xFF;
    if (!tlv_get(req, reqLen, 0x01, ctl, sizeof ctl, &ctlLen) || !tlv_get_u8(ctl, ctlLen, 0x02, &cmd))
        return kHAPError_InvalidData;

    HAPLogInfo(&logObject, "SelectedRTPStreamConfiguration %d: command %u.", slot->index, cmd);
    rtpdump_selected(slot->index, req, reqLen);
    switch (cmd) {
        case kCmd_Start: {
            if (StreamingDisabled(slot)) {
                HAPLogInfo(&logObject, "Start refused: streaming is disabled.");
                return kHAPError_InvalidState;
            }
            if (!slot->haveSetup || slot->stream) {
                HAPLogError(&logObject, "Start without a pending setup.");
                return kHAPError_InvalidState;
            }
            uint8_t vp[256], attrs[32], rtp[64];
            size_t vpLen, attrsLen, rtpLen;
            if (!tlv_get(req, reqLen, 0x02, vp, sizeof vp, &vpLen) ||
                !tlv_get(vp, vpLen, 0x03, attrs, sizeof attrs, &attrsLen) ||
                !tlv_get(vp, vpLen, 0x04, rtp, sizeof rtp, &rtpLen))
                return kHAPError_InvalidData;
            stream_params_t* p = &slot->pending;
            uint8_t fps = 0, pt = 99;
            tlv_get_u16(attrs, attrsLen, 0x01, &p->width);
            tlv_get_u16(attrs, attrsLen, 0x02, &p->height);
            tlv_get_u8(attrs, attrsLen, 0x03, &fps);
            p->fps = fps;
            tlv_get_u8(rtp, rtpLen, 0x01, &pt);
            p->payloadType = pt;
            tlv_get_u16(rtp, rtpLen, 0x03, &p->maxBitrateKbps);
            p->mtu = 0;
            tlv_get_u16(rtp, rtpLen, 0x05, &p->mtu);
            if (!p->mtu)
                p->mtu = p->videoDst.ss_family == AF_INET6 ? 1228 : 1378;
            p->ringName = ChooseVideoRing(slot, p);

            // Audio: 03 {01 codec, 02 params {01 channels, 02 bitrate mode, 03 sample rate, 04 rtp time},
            //            03 rtp {01 pt, 02 ssrc, 03 max bitrate, 04 rtcp interval}, 04 comfort noise}
            uint8_t ap[128], acp[32], artp[64];
            size_t apLen, acpLen = 0, artpLen = 0;
            uint8_t acodec = 0xFF, asr = 0xFF, aptime = 0, apt = 110;
            p->audioEnabled = false;
            if (tlv_get(req, reqLen, 0x03, ap, sizeof ap, &apLen)) {
                tlv_get_u8(ap, apLen, 0x01, &acodec);
                if (tlv_get(ap, apLen, 0x02, acp, sizeof acp, &acpLen)) {
                    tlv_get_u8(acp, acpLen, 0x03, &asr);
                    tlv_get_u8(acp, acpLen, 0x04, &aptime);
                }
                if (tlv_get(ap, apLen, 0x03, artp, sizeof artp, &artpLen))
                    tlv_get_u8(artp, artpLen, 0x01, &apt);
                p->audioPayloadType = apt;
                p->audioPacketMs = aptime;
                p->audioRingName = OpusRingName();
                // RTP clock: HomeKit times Opus by the negotiated sample rate (as Scrypted does), not RFC 7587's
                // 48 kHz. RHKD_AUDIO_CLOCK=48000 forces the RFC clock for A/B tests.
                p->audioClockHz = asr == 0 ? 8000 : asr == 2 ? 24000 : 16000;
                const char* clk = getenv("RHKD_AUDIO_CLOCK");
                if (clk && atoi(clk) >= 8000 && atoi(clk) <= 48000 && atoi(clk) % 1000 == 0)
                    p->audioClockHz = (uint32_t) atoi(clk);
                p->audioEnabled = acodec == 3; // Opus
                HAPLogInfo(&logObject, "Start audio: codec %u, sample rate idx %u, packet time %u ms, pt %u, RTP clock %u -> %s.",
                           acodec, asr, aptime, apt, (unsigned) p->audioClockHz, p->audioEnabled ? "sending" : "not supported, silent");
            }
            HAPLogInfo(
                    &logObject,
                    "Start %d: %ux%u@%u, max %u kbit/s, pt %u, mtu %u -> ring %s.",
                    slot->index,
                    p->width,
                    p->height,
                    p->fps,
                    p->maxBitrateKbps,
                    p->payloadType,
                    p->mtu,
                    p->ringName);
            slot->stream = stream_start(p); // takes ownership of the sockets
            stream_set_audio_muted(slot->stream, micMuted);
            if (!slot->stream) {
                CloseSetupSockets(slot);
                slot->haveSetup = false;
                SetStreamingStatus(slot, kStreamingStatus_Available);
                return kHAPError_Unknown;
            }
            slot->streamSession = request->session;
            bitrate_apply(slot->index, p->ringName, p->maxBitrateKbps);
            opmode_stream_started();
            if (reqLen <= sizeof slot->selected) {
                memcpy(slot->selected, req, reqLen);
                slot->selectedLen = reqLen;
            }
            break;
        }
        case kCmd_End:
            EndSession(slot, "controller ended session");
            break;
        case kCmd_Reconfigure: {
            // Same layout as Start: 02 {03 attributes {01 w, 02 h, 03 fps}, 04 rtp {03 max bitrate}}; all optional.
            uint8_t vp[256], attrs[32], rtp[64];
            size_t vpLen, attrsLen = 0, rtpLen = 0;
            uint16_t w = 0, h = 0, kbps = 0;
            uint8_t fps = 0;
            if (tlv_get(req, reqLen, 0x02, vp, sizeof vp, &vpLen)) {
                if (tlv_get(vp, vpLen, 0x03, attrs, sizeof attrs, &attrsLen)) {
                    tlv_get_u16(attrs, attrsLen, 0x01, &w);
                    tlv_get_u16(attrs, attrsLen, 0x02, &h);
                    tlv_get_u8(attrs, attrsLen, 0x03, &fps);
                }
                if (tlv_get(vp, vpLen, 0x04, rtp, sizeof rtp, &rtpLen))
                    tlv_get_u16(rtp, rtpLen, 0x03, &kbps);
            }
            // The source size is fixed per session (sub/aux/main as chosen at Start); rvd can't resize aux safely
            // yet, so only the bitrate follows.
            HAPLogInfo(&logObject, "Reconfigure %d: %ux%u@%u, max %u kbit/s.", slot->index, w, h, fps, kbps);
            if (slot->stream && kbps)
                bitrate_apply(slot->index, slot->pending.ringName, kbps);
            stream_request_idr(slot->stream);
            break;
        }
        case kCmd_Suspend: // e.g. the Home app went to the background: stop media, keep the session
        case kCmd_Resume:
            HAPLogInfo(&logObject, "%s %d.", cmd == kCmd_Suspend ? "Suspend" : "Resume", slot->index);
            stream_set_paused(slot->stream, cmd == kCmd_Suspend);
            break;
        default:
            break;
    }
    return kHAPError_None;
}

static HAPError HandleActiveRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPUInt8CharacteristicReadRequest* request,
        uint8_t* value,
        void* _Nullable context HAP_UNUSED) {
    *value = SlotFor(request->service)->active;
    return kHAPError_None;
}

static HAPError HandleActiveWrite(
        HAPAccessoryServerRef* server,
        const HAPUInt8CharacteristicWriteRequest* request,
        uint8_t value,
        void* _Nullable context HAP_UNUSED) {
    rtp_slot_t* slot = SlotFor(request->service);
    HAPLogInfo(&logObject, "Active %d <- %u", slot->index, value);
    if (slot->active != value) {
        slot->active = value;
        if (!value)
            EndSession(slot, "camera deactivated");
        HAPAccessoryServerRaiseEvent(server, request->characteristic, request->service, request->accessory);
    }
    return kHAPError_None;
}

// ---------------------------------------------------------------------------------------------------------------------
// Snapshot hook used by the patched ADK POST /resource handler.

size_t HAPCameraTakeSnapshot(uint16_t width, uint16_t height, int reason, uint8_t* bytes, size_t maxBytes) {
    HAPLogInfo(&logObject, "Snapshot requested %ux%u (reason %d).", width, height, reason);
    // As HAP-NodeJS CameraController.handleSnapshotRequest: no snapshots while streaming is off or the
    // operating mode forbids this kind of snapshot.
    if (!cam.slot[0].active || !opmode_snapshot_allowed(reason)) {
        HAPLogInfo(&logObject, "Snapshot refused by the operating mode.");
        return 0;
    }
    // The T41 main-resolution JPEG channel (jpeg0) produces no frames, so default to the
    // 640x360 substream JPEG; iOS scales it. Override with RHKD_SNAPSHOT_RING.
    const char* ring = getenv("RHKD_SNAPSHOT_RING");
    return snapshot_take(ring && *ring ? ring : "jpeg1", bytes, maxBytes, 2500);
}

// ---------------------------------------------------------------------------------------------------------------------
// Attribute database

#define kIID_Camera                 ((uint64_t) 0x0030)
#define kIID_CameraStreamingStatus  ((uint64_t) 0x0031)
#define kIID_CameraSupportedVideo   ((uint64_t) 0x0032)
#define kIID_CameraSupportedAudio   ((uint64_t) 0x0033)
#define kIID_CameraSupportedRTP     ((uint64_t) 0x0034)
#define kIID_CameraSelected         ((uint64_t) 0x0035)
#define kIID_CameraSetupEndpoints   ((uint64_t) 0x0036)
#define kIID_CameraActive           ((uint64_t) 0x0037)

#define TLV8_PROPS(r, w, ev) \
    { .readable = r, \
      .writable = w, \
      .supportsEventNotification = ev, \
      .hidden = false, \
      .requiresTimedWrite = false, \
      .supportsAuthorizationData = false, \
      .ip = { .controlPoint = false, .supportsWriteResponse = false }, \
      .ble = { .supportsBroadcastNotification = false, \
               .supportsDisconnectedNotification = false, \
               .readableWithoutSecurity = false, \
               .writableWithoutSecurity = false } }

static const HAPTLV8Characteristic streamingStatusCharacteristic = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_CameraStreamingStatus,
    .characteristicType = &kCharType_StreamingStatus,
    .debugDescription = "streaming-status",
    .properties = TLV8_PROPS(true, false, true),
    .callbacks = { .handleRead = HandleStreamingStatusRead },
};
static const HAPTLV8Characteristic supportedVideoCharacteristic = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_CameraSupportedVideo,
    .characteristicType = &kCharType_SupportedVideoStreamConfiguration,
    .debugDescription = "supported-video-stream-configuration",
    .properties = TLV8_PROPS(true, false, false),
    .callbacks = { .handleRead = HandleSupportedVideoRead },
};
static const HAPTLV8Characteristic supportedAudioCharacteristic = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_CameraSupportedAudio,
    .characteristicType = &kCharType_SupportedAudioStreamConfiguration,
    .debugDescription = "supported-audio-stream-configuration",
    .properties = TLV8_PROPS(true, false, false),
    .callbacks = { .handleRead = HandleSupportedAudioRead },
};
static const HAPTLV8Characteristic supportedRTPCharacteristic = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_CameraSupportedRTP,
    .characteristicType = &kCharType_SupportedRTPConfiguration,
    .debugDescription = "supported-rtp-configuration",
    .properties = TLV8_PROPS(true, false, false),
    .callbacks = { .handleRead = HandleSupportedRTPRead },
};
static const HAPTLV8Characteristic selectedCharacteristic = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_CameraSelected,
    .characteristicType = &kCharType_SelectedRTPStreamConfiguration,
    .debugDescription = "selected-rtp-stream-configuration",
    .properties = TLV8_PROPS(true, true, false),
    .callbacks = { .handleRead = HandleSelectedRead, .handleWrite = HandleSelectedWrite },
};
static const HAPTLV8Characteristic setupEndpointsCharacteristic = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_CameraSetupEndpoints,
    .characteristicType = &kCharType_SetupEndpoints,
    .debugDescription = "setup-endpoints",
    .properties = TLV8_PROPS(true, true, false),
    .callbacks = { .handleRead = HandleSetupEndpointsRead, .handleWrite = HandleSetupEndpointsWrite },
};
static const HAPUInt8Characteristic activeCharacteristic = {
    .format = kHAPCharacteristicFormat_UInt8,
    .iid = kIID_CameraActive,
    .characteristicType = &kHAPCharacteristicType_Active,
    .debugDescription = kHAPCharacteristicDebugDescription_Active,
    .properties = TLV8_PROPS(true, true, true),
    .units = kHAPCharacteristicUnits_None,
    .constraints = { .minimumValue = 0, .maximumValue = 1, .stepValue = 1 },
    .callbacks = { .handleRead = HandleActiveRead, .handleWrite = HandleActiveWrite },
};

static const HAPService cameraService = {
    .iid = kIID_Camera,
    .serviceType = &kHAPServiceType_CameraRTPStreamManagement,
    .debugDescription = kHAPServiceDebugDescription_CameraRTPStreamManagement,
    .name = NULL,
    .properties = { .primaryService = true, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &streamingStatusCharacteristic,
                                                            &supportedVideoCharacteristic,
                                                            &supportedAudioCharacteristic,
                                                            &supportedRTPCharacteristic,
                                                            &selectedCharacteristic,
                                                            &setupEndpointsCharacteristic,
                                                            &activeCharacteristic,
                                                            NULL },
};

// Second Camera RTP Stream Management service: a second live viewer at the same time (own session, own source).
#define kIID_Camera2                 ((uint64_t) 0x00A0)
#define kIID_Camera2StreamingStatus  ((uint64_t) 0x00A1)
#define kIID_Camera2SupportedVideo   ((uint64_t) 0x00A2)
#define kIID_Camera2SupportedAudio   ((uint64_t) 0x00A3)
#define kIID_Camera2SupportedRTP     ((uint64_t) 0x00A4)
#define kIID_Camera2Selected         ((uint64_t) 0x00A5)
#define kIID_Camera2SetupEndpoints   ((uint64_t) 0x00A6)
#define kIID_Camera2Active           ((uint64_t) 0x00A7)

static const HAPTLV8Characteristic streamingStatusCharacteristic2 = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_Camera2StreamingStatus,
    .characteristicType = &kCharType_StreamingStatus,
    .debugDescription = "streaming-status",
    .properties = TLV8_PROPS(true, false, true),
    .callbacks = { .handleRead = HandleStreamingStatusRead },
};
static const HAPTLV8Characteristic supportedVideoCharacteristic2 = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_Camera2SupportedVideo,
    .characteristicType = &kCharType_SupportedVideoStreamConfiguration,
    .debugDescription = "supported-video-stream-configuration",
    .properties = TLV8_PROPS(true, false, false),
    .callbacks = { .handleRead = HandleSupportedVideoRead },
};
static const HAPTLV8Characteristic supportedAudioCharacteristic2 = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_Camera2SupportedAudio,
    .characteristicType = &kCharType_SupportedAudioStreamConfiguration,
    .debugDescription = "supported-audio-stream-configuration",
    .properties = TLV8_PROPS(true, false, false),
    .callbacks = { .handleRead = HandleSupportedAudioRead },
};
static const HAPTLV8Characteristic supportedRTPCharacteristic2 = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_Camera2SupportedRTP,
    .characteristicType = &kCharType_SupportedRTPConfiguration,
    .debugDescription = "supported-rtp-configuration",
    .properties = TLV8_PROPS(true, false, false),
    .callbacks = { .handleRead = HandleSupportedRTPRead },
};
static const HAPTLV8Characteristic selectedCharacteristic2 = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_Camera2Selected,
    .characteristicType = &kCharType_SelectedRTPStreamConfiguration,
    .debugDescription = "selected-rtp-stream-configuration",
    .properties = TLV8_PROPS(true, true, false),
    .callbacks = { .handleRead = HandleSelectedRead, .handleWrite = HandleSelectedWrite },
};
static const HAPTLV8Characteristic setupEndpointsCharacteristic2 = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_Camera2SetupEndpoints,
    .characteristicType = &kCharType_SetupEndpoints,
    .debugDescription = "setup-endpoints",
    .properties = TLV8_PROPS(true, true, false),
    .callbacks = { .handleRead = HandleSetupEndpointsRead, .handleWrite = HandleSetupEndpointsWrite },
};
static const HAPUInt8Characteristic activeCharacteristic2 = {
    .format = kHAPCharacteristicFormat_UInt8,
    .iid = kIID_Camera2Active,
    .characteristicType = &kHAPCharacteristicType_Active,
    .debugDescription = kHAPCharacteristicDebugDescription_Active,
    .properties = TLV8_PROPS(true, true, true),
    .units = kHAPCharacteristicUnits_None,
    .constraints = { .minimumValue = 0, .maximumValue = 1, .stepValue = 1 },
    .callbacks = { .handleRead = HandleActiveRead, .handleWrite = HandleActiveWrite },
};

static const HAPService cameraService2 = {
    .iid = kIID_Camera2,
    .serviceType = &kHAPServiceType_CameraRTPStreamManagement,
    .debugDescription = kHAPServiceDebugDescription_CameraRTPStreamManagement,
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &streamingStatusCharacteristic2,
                                                            &supportedVideoCharacteristic2,
                                                            &supportedAudioCharacteristic2,
                                                            &supportedRTPCharacteristic2,
                                                            &selectedCharacteristic2,
                                                            &setupEndpointsCharacteristic2,
                                                            &activeCharacteristic2,
                                                            NULL },
};

// Microphone service: required on IP cameras by the HAP spec. Mute stops the audio packets.
#define kIID_Microphone     ((uint64_t) 0x0040)
#define kIID_MicrophoneMute ((uint64_t) 0x0041)



static HAPError HandleMuteRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPBoolCharacteristicReadRequest* request HAP_UNUSED,
        bool* value,
        void* _Nullable context HAP_UNUSED) {
    *value = micMuted;
    return kHAPError_None;
}

static HAPError HandleMuteWrite(
        HAPAccessoryServerRef* server,
        const HAPBoolCharacteristicWriteRequest* request,
        bool value,
        void* _Nullable context HAP_UNUSED) {
    HAPLogInfo(&logObject, "Microphone mute <- %d", value);
    if (micMuted != value) {
        micMuted = value;
        for (int i = 0; i < kRtpSlots; i++)
            stream_set_audio_muted(cam.slot[i].stream, value);
        HAPAccessoryServerRaiseEvent(server, request->characteristic, request->service, request->accessory);
    }
    return kHAPError_None;
}

static const HAPBoolCharacteristic muteCharacteristic = {
    .format = kHAPCharacteristicFormat_Bool,
    .iid = kIID_MicrophoneMute,
    .characteristicType = &kCharType_Mute,
    .debugDescription = "mute",
    .properties = TLV8_PROPS(true, true, true),
    .callbacks = { .handleRead = HandleMuteRead, .handleWrite = HandleMuteWrite },
};

static const HAPService microphoneService = {
    .iid = kIID_Microphone,
    .serviceType = &kHAPServiceType_Microphone,
    .debugDescription = kHAPServiceDebugDescription_Microphone,
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &muteCharacteristic, NULL },
};

// Speaker service: two-way audio. Its presence makes Home show the talk button; the controller's Opus then arrives
// on the session's audio socket and goes to rad's speaker ring (talkback.c). Mute and Volume act on that audio
// (Volume scales it; rad's ao_volume stays the overall level).
#define kIID_Speaker       ((uint64_t) 0x0090)
#define kIID_SpeakerMute   ((uint64_t) 0x0091)
#define kIID_SpeakerVolume ((uint64_t) 0x0092)
static const HAPUUID kCharType_Volume = HAPUUIDCreateAppleDefined(0x119);

static HAPError HandleSpeakerMuteRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPBoolCharacteristicReadRequest* request HAP_UNUSED,
        bool* value,
        void* _Nullable context HAP_UNUSED) {
    *value = talkback_muted();
    return kHAPError_None;
}

static HAPError HandleSpeakerMuteWrite(
        HAPAccessoryServerRef* server,
        const HAPBoolCharacteristicWriteRequest* request,
        bool value,
        void* _Nullable context HAP_UNUSED) {
    HAPLogInfo(&logObject, "Speaker mute <- %d", value);
    if (talkback_muted() != value) {
        talkback_set_muted(value);
        HAPAccessoryServerRaiseEvent(server, request->characteristic, request->service, request->accessory);
    }
    return kHAPError_None;
}

static HAPError HandleSpeakerVolumeRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPUInt8CharacteristicReadRequest* request HAP_UNUSED,
        uint8_t* value,
        void* _Nullable context HAP_UNUSED) {
    *value = talkback_volume();
    return kHAPError_None;
}

static HAPError HandleSpeakerVolumeWrite(
        HAPAccessoryServerRef* server,
        const HAPUInt8CharacteristicWriteRequest* request,
        uint8_t value,
        void* _Nullable context HAP_UNUSED) {
    HAPLogInfo(&logObject, "Speaker volume <- %u", value);
    if (talkback_volume() != value) {
        talkback_set_volume(value);
        HAPAccessoryServerRaiseEvent(server, request->characteristic, request->service, request->accessory);
    }
    return kHAPError_None;
}

static const HAPBoolCharacteristic speakerMuteCharacteristic = {
    .format = kHAPCharacteristicFormat_Bool,
    .iid = kIID_SpeakerMute,
    .characteristicType = &kCharType_Mute,
    .debugDescription = "mute",
    .properties = TLV8_PROPS(true, true, true),
    .callbacks = { .handleRead = HandleSpeakerMuteRead, .handleWrite = HandleSpeakerMuteWrite },
};

static const HAPUInt8Characteristic speakerVolumeCharacteristic = {
    .format = kHAPCharacteristicFormat_UInt8,
    .iid = kIID_SpeakerVolume,
    .characteristicType = &kCharType_Volume,
    .debugDescription = "volume",
    .properties = TLV8_PROPS(true, true, true),
    .units = kHAPCharacteristicUnits_Percentage,
    .constraints = { .minimumValue = 0, .maximumValue = 100, .stepValue = 1 },
    .callbacks = { .handleRead = HandleSpeakerVolumeRead, .handleWrite = HandleSpeakerVolumeWrite },
};

static const HAPService speakerService = {
    .iid = kIID_Speaker,
    .serviceType = &kHAPServiceType_Speaker,
    .debugDescription = kHAPServiceDebugDescription_Speaker,
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &speakerMuteCharacteristic,
                                                            &speakerVolumeCharacteristic,
                                                            NULL },
};

// "Echo Cancellation" switch: when on, rad's acoustic echo cancellation runs while someone talks through the camera,
// so the live view's microphone doesn't play the speaker's sound back (talkback.c). Kept in the key-value store.
#define kIID_EchoSwitch     ((uint64_t) 0x00B0)
#define kIID_EchoSwitchOn   ((uint64_t) 0x00B1)
#define kIID_EchoSwitchName ((uint64_t) 0x00B2)
#define kIID_EchoSwitchConfiguredName ((uint64_t) 0x00B3)
#define kAppKVSKey_EchoCancel ((HAPPlatformKeyValueStoreKey) 0x04) // domain 0x00; 0x01-0x03 are taken
#define kIID_MotionName           ((uint64_t) 0x0053)
#define kIID_MotionConfiguredName ((uint64_t) 0x0054)

// Home labels a non-primary service by its Configured Name; without one it shows the accessory's name ("Wyze v4"
// for the switch and the motion sensor). A rename in Home writes it; it is kept in the key-value store.
static const HAPUUID kCharType_ConfiguredName = HAPUUIDCreateAppleDefined(0xE3);
typedef struct {
    uint64_t nameIid, configuredIid;
    HAPPlatformKeyValueStoreKey key; // domain 0x00
    char name[65];
} service_name_t;
static service_name_t gNames[] = {
    { kIID_EchoSwitchName, kIID_EchoSwitchConfiguredName, 0x05, "Echo Cancellation" },
    { kIID_MotionName, kIID_MotionConfiguredName, 0x06, "Motion" },
};

// One motion sensor per detected class (classes.h), built at start from raptor.conf [motion].
// IIDs: 0x0400 + 0x10 * n for sensor n: +0 service, +1 MotionDetected, +2 StatusActive, +3 Name,
// +4 Configured Name. A rename is kept under KVS key 0x20 + class id, so it follows the class.
#define kIID_ClassBase ((uint64_t) 0x0400)
#define kIID_ClassStep ((uint64_t) 0x0010)
#define kAppKVSKey_ClassNameBase 0x20
static classes_t gClasses;
static service_name_t gClassNames[RHKD_MAX_CLASS_SENSORS];

static service_name_t* NameFor(uint64_t iid) {
    for (size_t i = 0; i < HAPArrayCount(gNames); i++)
        if (gNames[i].nameIid == iid || gNames[i].configuredIid == iid)
            return &gNames[i];
    for (int i = 0; i < gClasses.count; i++)
        if (gClassNames[i].nameIid == iid || gClassNames[i].configuredIid == iid)
            return &gClassNames[i];
    return NULL;
}

static HAPError HandleEchoOnRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPBoolCharacteristicReadRequest* request HAP_UNUSED,
        bool* value,
        void* _Nullable context HAP_UNUSED) {
    *value = talkback_echo_cancel();
    return kHAPError_None;
}

static HAPError HandleEchoOnWrite(
        HAPAccessoryServerRef* server,
        const HAPBoolCharacteristicWriteRequest* request,
        bool value,
        void* _Nullable context HAP_UNUSED) {
    HAPLogInfo(&logObject, "Echo cancellation <- %d", value);
    if (talkback_echo_cancel() != value) {
        talkback_set_echo_cancel(value);
        uint8_t b = value;
        if (HAPPlatformKeyValueStoreSet(cam.keyValueStore, (HAPPlatformKeyValueStoreDomain) 0x00,
                                        kAppKVSKey_EchoCancel, &b, 1))
            HAPLogError(&logObject, "Could not save the echo cancellation setting.");
        HAPAccessoryServerRaiseEvent(server, request->characteristic, request->service, request->accessory);
    }
    return kHAPError_None;
}

static HAPError HandleServiceNameRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPStringCharacteristicReadRequest* request,
        char* value,
        size_t maxValueBytes,
        void* _Nullable context HAP_UNUSED) {
    service_name_t* e = NameFor(request->characteristic->iid);
    if (!e)
        return kHAPError_Unknown;
    size_t n = strlen(e->name) + 1;
    if (maxValueBytes < n)
        return kHAPError_OutOfResources;
    memcpy(value, e->name, n);
    return kHAPError_None;
}

static HAPError HandleServiceNameWrite(
        HAPAccessoryServerRef* server,
        const HAPStringCharacteristicWriteRequest* request,
        const char* value,
        void* _Nullable context HAP_UNUSED) {
    service_name_t* e = NameFor(request->characteristic->iid);
    size_t n = strlen(value);
    if (!e || n == 0 || n >= sizeof e->name)
        return kHAPError_InvalidData;
    if (strcmp(value, e->name) == 0)
        return kHAPError_None;
    memcpy(e->name, value, n + 1);
    HAPLogInfo(&logObject, "Service %llu renamed to \"%s\".", (unsigned long long) request->service->iid, e->name);
    if (HAPPlatformKeyValueStoreSet(cam.keyValueStore, (HAPPlatformKeyValueStoreDomain) 0x00, e->key, e->name, n))
        HAPLogError(&logObject, "Could not save the service name.");
    HAPAccessoryServerRaiseEvent(server, request->characteristic, request->service, request->accessory);
    return kHAPError_None;
}

static void LoadName(service_name_t* e) {
    char name[sizeof e->name];
    size_t n = 0;
    bool found = false;
    if (!HAPPlatformKeyValueStoreGet(cam.keyValueStore, (HAPPlatformKeyValueStoreDomain) 0x00, e->key, name,
                                     sizeof name - 1, &n, &found) && found && n > 0) {
        name[n] = 0;
        memcpy(e->name, name, n + 1);
    }
}

static void LoadServiceNames(void) {
    for (size_t i = 0; i < HAPArrayCount(gNames); i++)
        LoadName(&gNames[i]);
    for (int i = 0; i < gClasses.count; i++)
        LoadName(&gClassNames[i]);
}

static const HAPBoolCharacteristic echoOnCharacteristic = {
    .format = kHAPCharacteristicFormat_Bool,
    .iid = kIID_EchoSwitchOn,
    .characteristicType = &kHAPCharacteristicType_On,
    .debugDescription = kHAPCharacteristicDebugDescription_On,
    .properties = TLV8_PROPS(true, true, true),
    .callbacks = { .handleRead = HandleEchoOnRead, .handleWrite = HandleEchoOnWrite },
};

static const HAPStringCharacteristic echoNameCharacteristic = {
    .format = kHAPCharacteristicFormat_String,
    .iid = kIID_EchoSwitchName,
    .characteristicType = &kHAPCharacteristicType_Name,
    .debugDescription = kHAPCharacteristicDebugDescription_Name,
    .properties = TLV8_PROPS(true, false, false),
    .constraints = { .maxLength = 64 },
    .callbacks = { .handleRead = HandleServiceNameRead },
};

static const HAPStringCharacteristic echoConfiguredNameCharacteristic = {
    .format = kHAPCharacteristicFormat_String,
    .iid = kIID_EchoSwitchConfiguredName,
    .characteristicType = &kCharType_ConfiguredName,
    .debugDescription = "configured-name",
    .properties = TLV8_PROPS(true, true, true),
    .constraints = { .maxLength = 64 },
    .callbacks = { .handleRead = HandleServiceNameRead, .handleWrite = HandleServiceNameWrite },
};

static const HAPService echoSwitchService = {
    .iid = kIID_EchoSwitch,
    .serviceType = &kHAPServiceType_Switch,
    .debugDescription = kHAPServiceDebugDescription_Switch,
    .name = "Echo Cancellation",
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &echoOnCharacteristic, &echoNameCharacteristic,
                                                               &echoConfiguredNameCharacteristic, NULL },
};

static void LoadEchoCancel(void) {
    uint8_t b = 0;
    size_t n = 0;
    bool found = false;
    if (!HAPPlatformKeyValueStoreGet(cam.keyValueStore, (HAPPlatformKeyValueStoreDomain) 0x00, kAppKVSKey_EchoCancel,
                                     &b, 1, &n, &found) && found && n == 1)
        talkback_set_echo_cancel(b != 0);
}

// Motion sensor: fed by rmd (debounced, with its cooldown) or rvd's raw IVS flag. StatusActive is false while
// neither answers, so Home shows the sensor as not responding instead of "no motion".
#define kIID_Motion             ((uint64_t) 0x0050)
#define kIID_MotionDetected     ((uint64_t) 0x0051)
#define kIID_MotionStatusActive ((uint64_t) 0x0052)

static HAPError HandleMotionDetectedRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPBoolCharacteristicReadRequest* request HAP_UNUSED,
        bool* value,
        void* _Nullable context HAP_UNUSED) {
    *value = motion_detected();
    return kHAPError_None;
}

static HAPError HandleMotionStatusActiveRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPBoolCharacteristicReadRequest* request HAP_UNUSED,
        bool* value,
        void* _Nullable context HAP_UNUSED) {
    *value = motion_available() && opmode_homekit_active(); // HAP-NodeJS mirrors HomeKitCameraActive here
    return kHAPError_None;
}

static const HAPBoolCharacteristic motionDetectedCharacteristic = {
    .format = kHAPCharacteristicFormat_Bool,
    .iid = kIID_MotionDetected,
    .characteristicType = &kHAPCharacteristicType_MotionDetected,
    .debugDescription = kHAPCharacteristicDebugDescription_MotionDetected,
    .properties = TLV8_PROPS(true, false, true),
    .callbacks = { .handleRead = HandleMotionDetectedRead },
};

static const HAPBoolCharacteristic motionStatusActiveCharacteristic = {
    .format = kHAPCharacteristicFormat_Bool,
    .iid = kIID_MotionStatusActive,
    .characteristicType = &kHAPCharacteristicType_StatusActive,
    .debugDescription = kHAPCharacteristicDebugDescription_StatusActive,
    .properties = TLV8_PROPS(true, false, true),
    .callbacks = { .handleRead = HandleMotionStatusActiveRead },
};

static const HAPStringCharacteristic motionNameCharacteristic = {
    .format = kHAPCharacteristicFormat_String,
    .iid = kIID_MotionName,
    .characteristicType = &kHAPCharacteristicType_Name,
    .debugDescription = kHAPCharacteristicDebugDescription_Name,
    .properties = TLV8_PROPS(true, false, false),
    .constraints = { .maxLength = 64 },
    .callbacks = { .handleRead = HandleServiceNameRead },
};

static const HAPStringCharacteristic motionConfiguredNameCharacteristic = {
    .format = kHAPCharacteristicFormat_String,
    .iid = kIID_MotionConfiguredName,
    .characteristicType = &kCharType_ConfiguredName,
    .debugDescription = "configured-name",
    .properties = TLV8_PROPS(true, true, true),
    .constraints = { .maxLength = 64 },
    .callbacks = { .handleRead = HandleServiceNameRead, .handleWrite = HandleServiceNameWrite },
};

static HAPError HandleClassDetectedRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPBoolCharacteristicReadRequest* request,
        bool* value,
        void* _Nullable context HAP_UNUSED) {
    *value = motion_class_detected((int) ((request->service->iid - kIID_ClassBase) / kIID_ClassStep));
    return kHAPError_None;
}

static HAPError HandleClassStatusActiveRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPBoolCharacteristicReadRequest* request HAP_UNUSED,
        bool* value,
        void* _Nullable context HAP_UNUSED) {
    *value = motion_class_available() && opmode_homekit_active();
    return kHAPError_None;
}

static HAPBoolCharacteristic gClassDetected[RHKD_MAX_CLASS_SENSORS];
static HAPBoolCharacteristic gClassActive[RHKD_MAX_CLASS_SENSORS];
static HAPStringCharacteristic gClassName[RHKD_MAX_CLASS_SENSORS];
static HAPStringCharacteristic gClassConfiguredName[RHKD_MAX_CLASS_SENSORS];
static const HAPCharacteristic* gClassChars[RHKD_MAX_CLASS_SENSORS][5];
static HAPService gClassService[RHKD_MAX_CLASS_SENSORS];

static const HAPService motionService = {
    .iid = kIID_Motion,
    .serviceType = &kHAPServiceType_MotionSensor,
    .debugDescription = kHAPServiceDebugDescription_MotionSensor,
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &motionDetectedCharacteristic,
                                                            &motionStatusActiveCharacteristic,
                                                            &motionNameCharacteristic,
                                                            &motionConfiguredNameCharacteristic,
                                                            NULL },
};

// ---------------------------------------------------------------------------------------------------------------------
// App glue (interface expected by ADK Applications/Main.c)

HAP_RESULT_USE_CHECK
HAPError IdentifyAccessory(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPAccessoryIdentifyRequest* request HAP_UNUSED,
        void* _Nullable context HAP_UNUSED) {
    HAPLogInfo(&logObject, "Identify");
    return kHAPError_None;
}

// The main sensor's characteristics, as the template for the class sensors.
static void BuildClassSensors(void) {
    for (int i = 0; i < gClasses.count; i++) {
        uint64_t base = kIID_ClassBase + kIID_ClassStep * (uint64_t) i;
        gClassDetected[i] = motionDetectedCharacteristic;
        gClassDetected[i].iid = base + 1;
        gClassDetected[i].callbacks.handleRead = HandleClassDetectedRead;
        gClassActive[i] = motionStatusActiveCharacteristic;
        gClassActive[i].iid = base + 2;
        gClassActive[i].callbacks.handleRead = HandleClassStatusActiveRead;
        gClassName[i] = motionNameCharacteristic;
        gClassName[i].iid = base + 3;
        gClassConfiguredName[i] = motionConfiguredNameCharacteristic;
        gClassConfiguredName[i].iid = base + 4;
        gClassChars[i][0] = &gClassDetected[i];
        gClassChars[i][1] = &gClassActive[i];
        gClassChars[i][2] = &gClassName[i];
        gClassChars[i][3] = &gClassConfiguredName[i];
        gClassChars[i][4] = NULL;
        gClassService[i] = motionService;
        gClassService[i].iid = base;
        gClassService[i].characteristics = gClassChars[i];
        service_name_t* e = &gClassNames[i];
        e->nameIid = base + 3;
        e->configuredIid = base + 4;
        e->key = (HAPPlatformKeyValueStoreKey) (kAppKVSKey_ClassNameBase + gClasses.id[i]);
        HAPRawBufferCopyBytes(e->name, gClasses.name[i], strlen(gClasses.name[i]) + 1);
    }
}

static const HAPService* gServices[12 + RHKD_MAX_CLASS_SENSORS + 1];

static HAPAccessory accessory = {
    .aid = 1,
    .category = kHAPAccessoryCategory_IPCameras,
    .name = "Thingino Camera",
    .manufacturer = "Thingino",
    .model = "rhkd",
    .serialNumber = "0000000001",
    .firmwareVersion = RHKD_VERSION,
    .hardwareVersion = "1",
    .services = gServices, // BuildServices()
    .callbacks = { .identify = IdentifyAccessory },
};

static void Watchdog(HAPPlatformTimerRef timer HAP_UNUSED, void* _Nullable context HAP_UNUSED) {
    for (int i = 0; i < kRtpSlots; i++)
        if (cam.slot[i].stream && stream_finished(cam.slot[i].stream))
            EndSession(&cam.slot[i], "stream thread ended");
    HAPError err = HAPPlatformTimerRegister(&cam.watchdog, HAPPlatformClockGetCurrent() + 2000, Watchdog, NULL);
    HAPAssert(!err);
}

static void HandleSessionInvalidate(HAPAccessoryServerRef* server HAP_UNUSED, HAPSessionRef* session, void* _Nullable ctx HAP_UNUSED) {
    hds_session_invalidated(session);
    for (int i = 0; i < kRtpSlots; i++) {
        rtp_slot_t* s = &cam.slot[i];
        if (s->stream && session == s->streamSession)
            EndSession(s, "HAP session closed");
        else if (!s->stream && s->haveSetup && session == s->setupSession)
            EndSession(s, "HAP session closed before start");
    }
}

static void BuildServices(void) {
    static const HAPService* const fixed[] = { &accessoryInformationService, &hapProtocolInformationService,
                                               &pairingService,              &cameraService,
                                               &cameraService2,              &microphoneService,
                                               &speakerService,              &echoSwitchService,
                                               &motionService,               &operatingModeService,
                                               &dataStreamService,           &recordingService };
    size_t n = 0;
    for (size_t i = 0; i < HAPArrayCount(fixed); i++)
        gServices[n++] = fixed[i];
    for (int i = 0; i < gClasses.count; i++)
        gServices[n++] = &gClassService[i];
    gServices[n] = NULL;
}

void AppCreate(HAPAccessoryServerRef* server, HAPPlatformKeyValueStoreRef keyValueStore) {
    HAPRawBufferZero(&cam, sizeof cam);
    cam.server = server;
    cam.keyValueStore = keyValueStore;
    static const HAPService* const services[kRtpSlots] = { &cameraService, &cameraService2 };
    static const HAPCharacteristic* const status[kRtpSlots] = {
        (const HAPCharacteristic*) &streamingStatusCharacteristic, (const HAPCharacteristic*) &streamingStatusCharacteristic2
    };
    static const HAPCharacteristic* const active[kRtpSlots] = {
        (const HAPCharacteristic*) &activeCharacteristic, (const HAPCharacteristic*) &activeCharacteristic2
    };
    for (int i = 0; i < kRtpSlots; i++) {
        rtp_slot_t* s = &cam.slot[i];
        s->index = i;
        s->service = services[i];
        s->statusCharacteristic = status[i];
        s->activeCharacteristic = active[i];
        s->active = 1;
        s->streamingStatus = kStreamingStatus_Available;
        s->pending.videoSock = s->pending.audioSock = -1;
    }
    const char* name = getenv("RHKD_NAME");
    if (name && *name)
        accessory.name = name;
    const char* serial = getenv("RHKD_SERIAL");
    if (serial && *serial)
        accessory.serialNumber = serial;
    HAPLogInfo(&logObject, "rhkd %s, camera accessory \"%s\"", RHKD_VERSION, accessory.name);
}

void AppRelease(void) {
    EndAllSessions("shutdown");
    talkback_shutdown();
}

// Bump this whenever services or characteristics change, so controllers re-read /accessories.
#define kDBRevision ((uint32_t) 12) // 9: second RTP stream service, Echo Cancellation switch; 10: its Configured Name;
                                     // 11: motion sensor Name + Configured Name; 12: class sensors
// The class sensors come from raptor.conf, so their set is part of the revision: kDBRevision << 16 | fingerprint.
#define kAppKVSDomain ((HAPPlatformKeyValueStoreDomain) 0x00)
#define kAppKVSKey_DBRevision ((HAPPlatformKeyValueStoreKey) 0x01)

// ADK-internal (HAPAccessoryServer+Internal.h), not exported via HAP.h.
HAPError HAPAccessoryServerIncrementCN(HAPPlatformKeyValueStoreRef keyValueStore);

static void BumpConfigurationNumberIfDBChanged(void) {
    uint8_t bytes[4];
    size_t n = 0;
    bool found = false;
    if (HAPPlatformKeyValueStoreGet(cam.keyValueStore, kAppKVSDomain, kAppKVSKey_DBRevision, bytes, sizeof bytes, &n, &found))
        return;
    uint32_t revision = kDBRevision << 16 | classes_fingerprint(&gClasses);
    if (found && n == sizeof bytes && HAPReadLittleUInt32(bytes) == revision)
        return;
    HAPLogInfo(&logObject, "Accessory database revision %lu.%04x is new: incrementing configuration number.",
               (unsigned long) kDBRevision, (unsigned) (revision & 0xFFFF));
    if (HAPAccessoryServerIncrementCN(cam.keyValueStore))
        return;
    HAPWriteLittleUInt32(bytes, revision);
    (void) HAPPlatformKeyValueStoreSet(cam.keyValueStore, kAppKVSDomain, kAppKVSKey_DBRevision, bytes, sizeof bytes);
}

// Runs on the HAP run loop (scheduled by the motion thread).
static void MotionChanged(bool motion, bool available) {
    if (!cam.server)
        return;
    HAPAccessoryServerRaiseEvent(cam.server, (const HAPCharacteristic*) &motionDetectedCharacteristic, &motionService,
                                 AppGetAccessoryInfo());
    HAPAccessoryServerRaiseEvent(cam.server, (const HAPCharacteristic*) &motionStatusActiveCharacteristic,
                                 &motionService, AppGetAccessoryInfo());
    (void) motion;
    (void) available;
}

// Runs on the HAP run loop (scheduled by the motion thread).
static void ClassMotionChanged(int idx, bool motion HAP_UNUSED, bool available HAP_UNUSED) {
    if (!cam.server || idx < 0 || idx >= gClasses.count)
        return;
    HAPAccessoryServerRaiseEvent(cam.server, (const HAPCharacteristic*) &gClassDetected[idx], &gClassService[idx],
                                 AppGetAccessoryInfo());
    HAPAccessoryServerRaiseEvent(cam.server, (const HAPCharacteristic*) &gClassActive[idx], &gClassService[idx],
                                 AppGetAccessoryInfo());
}

// Runs on the HAP run loop when the camera is turned on or off in Home.
static void OperatingModeChanged(bool homeKitActive) {
    if (!homeKitActive) {
        EndAllSessions("camera turned off in Home");
        recording_camera_disabled();
    }
    // StatusActive mirrors HomeKitCameraActive; StreamingStatus/SetupEndpoints reads change with it.
    MotionChanged(motion_detected(), motion_available());
    for (int i = 0; i < gClasses.count; i++)
        ClassMotionChanged(i, motion_class_detected(i), motion_class_available());
}

void AppAccessoryServerStart(void) {
    const char* conf = getenv("RHKD_RAPTOR_CONF");
    classes_load(&gClasses, conf && *conf ? conf : "/etc/raptor.conf");
    BuildClassSensors();
    BuildServices();
    if (gClasses.count) {
        char list[RHKD_MAX_CLASS_SENSORS * 34] = "";
        for (int i = 0; i < gClasses.count; i++)
            snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", gClasses.name[i]);
        HAPLogInfo(&logObject, "%d class sensor(s): %s; HKSV records on %s", gClasses.count, list,
                   gClasses.recordAll ? "every class" : "record_classes");
    }
    BumpConfigurationNumberIfDBChanged();
    LoadEchoCancel();
    LoadServiceNames();
    opmode_init(cam.server, cam.keyValueStore, OperatingModeChanged);
    recording_init(cam.server, cam.keyValueStore);
    hds_init(recording_hds_message, recording_hds_closed);
    bitrate_recover();
    HAPAccessoryServerStart(cam.server, &accessory);
    if (motion_start(MotionChanged, ClassMotionChanged, &gClasses, 500))
        HAPLogError(&logObject, "could not start motion polling");
    HAPError err = HAPPlatformTimerRegister(&cam.watchdog, HAPPlatformClockGetCurrent() + 2000, Watchdog, NULL);
    HAPAssert(!err);
}

void AccessoryServerHandleUpdatedState(HAPAccessoryServerRef* server, void* _Nullable context HAP_UNUSED) {
    HAPLogInfo(&logObject, "Accessory server state: %d", (int) HAPAccessoryServerGetState(server));
}

const HAPAccessory* AppGetAccessoryInfo(void) {
    return &accessory;
}

void AppInitialize(
        HAPAccessoryServerOptions* options HAP_UNUSED,
        HAPPlatform* platform HAP_UNUSED,
        HAPAccessoryServerCallbacks* callbacks) {
    callbacks->handleSessionInvalidate = HandleSessionInvalidate;
}

void AppDeinitialize(void) {
}
