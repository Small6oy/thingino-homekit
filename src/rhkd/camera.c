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

#include "App.h"
#include "DB.h"
#include "bitrate.h"
#include "snapshot.h"
#include "stream.h"
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

static struct {
    HAPAccessoryServerRef* server;
    HAPPlatformKeyValueStoreRef keyValueStore;
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
    HAPPlatformTimerRef watchdog;
} cam;

extern const HAPAccessory* AppGetAccessoryInfo(void);
static const HAPService cameraService;
static const HAPTLV8Characteristic streamingStatusCharacteristic;

// Microphone mute (Microphone service). When muted, audio packets are not sent.
static bool micMuted = false;

// ---------------------------------------------------------------------------------------------------------------------
// Helpers

// Streaming is refused while the RTP service is inactive or the camera is turned off in Home (HomeKitCameraActive),
// as HAP-NodeJS RTPStreamManagement.streamingIsDisabled does.
static bool StreamingDisabled(void) {
    return !cam.active || !opmode_homekit_active();
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

static void SetStreamingStatus(uint8_t status) {
    if (cam.streamingStatus == status)
        return;
    cam.streamingStatus = status;
    if (cam.server)
        HAPAccessoryServerRaiseEvent(
                cam.server,
                (const HAPCharacteristic*) &streamingStatusCharacteristic,
                &cameraService,
                AppGetAccessoryInfo());
}

static void CloseSetupSockets(void) {
    if (cam.pending.videoSock >= 0)
        close(cam.pending.videoSock);
    if (cam.pending.audioSock >= 0)
        close(cam.pending.audioSock);
    cam.pending.videoSock = cam.pending.audioSock = -1;
}

static void EndSession(const char* why) {
    if (cam.stream) {
        HAPLogInfo(&logObject, "Ending stream (%s).", why);
        stream_stop(cam.stream); // closes the sockets it owns
        cam.stream = NULL;
    } else if (cam.haveSetup) {
        CloseSetupSockets();
    }
    cam.haveSetup = false;
    cam.streamSession = NULL;
    cam.setupSession = NULL;
    cam.selectedLen = 0;
    bitrate_restore();
    opmode_stream_ended();
    SetStreamingStatus(kStreamingStatus_Available);
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

static size_t BuildSupportedVideo(uint8_t* out, size_t cap) {
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
    for (size_t i = 0; i < HAPArrayCount(kResolutions); i++) {
        uint8_t a[16];
        tlvb_t ab;
        tlvb_init(&ab, a, sizeof a);
        tlvb_u16(&ab, 0x01, kResolutions[i][0]);
        tlvb_u16(&ab, 0x02, kResolutions[i][1]);
        tlvb_u8(&ab, 0x03, (uint8_t) kResolutions[i][2]);
        if (i)
            tlvb_sep(&b);
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

static HAPError HandleSupportedVideoRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request HAP_UNUSED,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    uint8_t buf[1024];
    size_t n = BuildSupportedVideo(buf, sizeof buf);
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
        const HAPTLV8CharacteristicReadRequest* request HAP_UNUSED,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    uint8_t st = cam.active ? cam.streamingStatus : kStreamingStatus_Unavailable;
    return HAPTLVWriterAppend(w, &(const HAPTLV) { .type = 0x01, .value = { .bytes = &st, .numBytes = 1 } });
}

static HAPError HandleSetupEndpointsRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request HAP_UNUSED,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    if (StreamingDisabled()) {
        uint8_t status = kSetupStatus_Error; // as HAP-NodeJS while streaming is disabled
        return HAPTLVWriterAppend(w, &(const HAPTLV) { .type = 0x02, .value = { .bytes = &status, .numBytes = 1 } });
    }
    if (!cam.setupResponseLen)
        return kHAPError_None; // empty until a controller sets up endpoints (as HAP-NodeJS does)
    return EmitRaw(w, cam.setupResponse, cam.setupResponseLen);
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
    if (StreamingDisabled()) {
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
    if (cam.stream && stream_finished(cam.stream))
        EndSession("finished");

    tlvb_t b;
    if (cam.stream) {
        HAPLogInfo(&logObject, "SetupEndpoints: busy.");
        tlvb_init(&b, cam.setupResponse, sizeof cam.setupResponse);
        tlvb_put(&b, 0x01, sid, 16);
        tlvb_u8(&b, 0x02, kSetupStatus_Busy);
        cam.setupResponseLen = b.len;
        return kHAPError_None;
    }
    if (cam.haveSetup) // a previous setup that never started
        CloseSetupSockets();

    uint8_t ver = 0;
    uint16_t vport = 0, aport = 0;
    char ip[64] = { 0 };
    size_t ipLen = 0;
    tlv_get_u8(addr, addrLen, 0x01, &ver);
    tlv_get(addr, addrLen, 0x02, ip, sizeof ip - 1, &ipLen);
    tlv_get_u16(addr, addrLen, 0x03, &vport);
    tlv_get_u16(addr, addrLen, 0x04, &aport);
    ip[ipLen] = '\0';

    stream_params_t* p = &cam.pending;
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
        CloseSetupSockets();
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

    tlvb_init(&b, cam.setupResponse, sizeof cam.setupResponse);
    tlvb_put(&b, 0x01, sid, 16);
    tlvb_u8(&b, 0x02, kSetupStatus_Success);
    tlvb_put(&b, 0x03, aaddr, aaddrLen);
    tlvb_put(&b, 0x04, vs, vsLen);
    tlvb_put(&b, 0x05, as, asLen);
    tlvb_u32(&b, 0x06, p->videoSSRC);
    tlvb_u32(&b, 0x07, p->audioSSRC);
    cam.setupResponseLen = b.len;

    memcpy(cam.sessionId, sid, 16);
    cam.haveSetup = true;
    cam.setupSession = request->session;
    HAPLogInfo(&logObject, "SetupEndpoints: controller %s:%u (video) -> us %s:%u.", ip, vport, myIp, myVport);
    SetStreamingStatus(kStreamingStatus_InUse);
    return kHAPError_None;
}

static HAPError HandleSelectedRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request HAP_UNUSED,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    if (StreamingDisabled()) {
        // HAP-NodeJS answers {01 session control {02 command = suspend}} while streaming is disabled.
        const uint8_t suspend[] = { 0x02, 0x01, kCmd_Suspend };
        return HAPTLVWriterAppend(w, &(const HAPTLV) { .type = 0x01, .value = { .bytes = suspend, .numBytes = sizeof suspend } });
    }
    if (!cam.selectedLen)
        return kHAPError_None; // empty until a stream is selected (as HAP-NodeJS does)
    return EmitRaw(w, cam.selected, cam.selectedLen);
}

static HAPError HandleSelectedWrite(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicWriteRequest* request,
        HAPTLVReaderRef* r,
        void* _Nullable context HAP_UNUSED) {
    uint8_t req[512];
    size_t reqLen;
    if (!CollectWrite(r, req, sizeof req, &reqLen))
        return kHAPError_InvalidData;

    uint8_t ctl[64];
    size_t ctlLen;
    uint8_t cmd = 0xFF;
    if (!tlv_get(req, reqLen, 0x01, ctl, sizeof ctl, &ctlLen) || !tlv_get_u8(ctl, ctlLen, 0x02, &cmd))
        return kHAPError_InvalidData;

    HAPLogInfo(&logObject, "SelectedRTPStreamConfiguration: command %u.", cmd);
    switch (cmd) {
        case kCmd_Start: {
            if (StreamingDisabled()) {
                HAPLogInfo(&logObject, "Start refused: streaming is disabled.");
                return kHAPError_InvalidState;
            }
            if (!cam.haveSetup || cam.stream) {
                HAPLogError(&logObject, "Start without a pending setup.");
                return kHAPError_InvalidState;
            }
            uint8_t vp[256], attrs[32], rtp[64];
            size_t vpLen, attrsLen, rtpLen;
            if (!tlv_get(req, reqLen, 0x02, vp, sizeof vp, &vpLen) ||
                !tlv_get(vp, vpLen, 0x03, attrs, sizeof attrs, &attrsLen) ||
                !tlv_get(vp, vpLen, 0x04, rtp, sizeof rtp, &rtpLen))
                return kHAPError_InvalidData;
            stream_params_t* p = &cam.pending;
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
            // Pick the source stream. iOS states its budget in max bitrate: on the LAN it asks for ~300 kbit/s
            // even at 1280x720. main (2560x1440, ~3 Mbit/s) is 10x over that; sub (640x360, ~1 Mbit/s) is
            // much closer. RHKD_VIDEO_RING=main|sub forces one; default "auto" chooses by bitrate.
            {
                const char* force = getenv("RHKD_VIDEO_RING");
                if (force && (strcmp(force, "main") == 0 || strcmp(force, "sub") == 0))
                    p->ringName = strcmp(force, "main") == 0 ? "main" : "sub";
                else
                    p->ringName = (p->maxBitrateKbps && p->maxBitrateKbps < 1000) || p->width <= 640 ? "sub" : "main";
                // HomeKit takes H.264 only. If the chosen stream is H.265 (e.g. main kept on H.265 for recording),
                // use the other one when it is H.264; if neither is, the stream thread logs why and stops.
                if (stream_ring_codec(p->ringName) != 0) {
                    const char* other = strcmp(p->ringName, "main") == 0 ? "sub" : "main";
                    if (stream_ring_codec(other) == 0) {
                        HAPLogInfo(&logObject, "Ring %s is not H.264; using %s instead.", p->ringName, other);
                        p->ringName = other;
                    }
                }
            }

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
                p->audioRingName = "audio";
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
                    "Start: %ux%u@%u, max %u kbit/s, pt %u, mtu %u -> ring %s.",
                    p->width,
                    p->height,
                    p->fps,
                    p->maxBitrateKbps,
                    p->payloadType,
                    p->mtu,
                    p->ringName);
            cam.stream = stream_start(p); // takes ownership of the sockets
            stream_set_audio_muted(cam.stream, micMuted);
            if (!cam.stream) {
                CloseSetupSockets();
                cam.haveSetup = false;
                SetStreamingStatus(kStreamingStatus_Available);
                return kHAPError_Unknown;
            }
            cam.streamSession = request->session;
            bitrate_apply(p->ringName, p->maxBitrateKbps);
            opmode_stream_started();
            if (reqLen <= sizeof cam.selected) {
                memcpy(cam.selected, req, reqLen);
                cam.selectedLen = reqLen;
            }
            break;
        }
        case kCmd_End:
            EndSession("controller ended session");
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
            HAPLogInfo(&logObject, "Reconfigure: %ux%u@%u, max %u kbit/s.", w, h, fps, kbps);
            if (cam.stream && kbps)
                bitrate_apply(cam.pending.ringName, kbps);
            stream_request_idr(cam.stream);
            break;
        }
        case kCmd_Suspend:
        case kCmd_Resume:
        default:
            break;
    }
    return kHAPError_None;
}

static HAPError HandleActiveRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPUInt8CharacteristicReadRequest* request HAP_UNUSED,
        uint8_t* value,
        void* _Nullable context HAP_UNUSED) {
    *value = cam.active;
    return kHAPError_None;
}

static HAPError HandleActiveWrite(
        HAPAccessoryServerRef* server,
        const HAPUInt8CharacteristicWriteRequest* request,
        uint8_t value,
        void* _Nullable context HAP_UNUSED) {
    HAPLogInfo(&logObject, "Active <- %u", value);
    if (cam.active != value) {
        cam.active = value;
        if (!value)
            EndSession("camera deactivated");
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
    if (!cam.active || !opmode_snapshot_allowed(reason)) {
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
        stream_set_audio_muted(cam.stream, value);
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

static const HAPService motionService = {
    .iid = kIID_Motion,
    .serviceType = &kHAPServiceType_MotionSensor,
    .debugDescription = kHAPServiceDebugDescription_MotionSensor,
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &motionDetectedCharacteristic,
                                                            &motionStatusActiveCharacteristic,
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

static HAPAccessory accessory = {
    .aid = 1,
    .category = kHAPAccessoryCategory_IPCameras,
    .name = "Thingino Camera",
    .manufacturer = "Thingino",
    .model = "rhkd",
    .serialNumber = "0000000001",
    .firmwareVersion = RHKD_VERSION,
    .hardwareVersion = "1",
    .services = (const HAPService* const[]) { &accessoryInformationService,
                                              &hapProtocolInformationService,
                                              &pairingService,
                                              &cameraService,
                                              &microphoneService,
                                              &motionService,
                                              &operatingModeService,
                                              &dataStreamService,
                                              &recordingService,
                                              NULL },
    .callbacks = { .identify = IdentifyAccessory },
};

static void Watchdog(HAPPlatformTimerRef timer HAP_UNUSED, void* _Nullable context HAP_UNUSED) {
    if (cam.stream && stream_finished(cam.stream))
        EndSession("stream thread ended");
    HAPError err = HAPPlatformTimerRegister(&cam.watchdog, HAPPlatformClockGetCurrent() + 2000, Watchdog, NULL);
    HAPAssert(!err);
}

static void HandleSessionInvalidate(HAPAccessoryServerRef* server HAP_UNUSED, HAPSessionRef* session, void* _Nullable ctx HAP_UNUSED) {
    hds_session_invalidated(session);
    if (cam.stream && session == cam.streamSession)
        EndSession("HAP session closed");
    else if (!cam.stream && cam.haveSetup && session == cam.setupSession)
        EndSession("HAP session closed before start");
}

void AppCreate(HAPAccessoryServerRef* server, HAPPlatformKeyValueStoreRef keyValueStore) {
    HAPRawBufferZero(&cam, sizeof cam);
    cam.server = server;
    cam.keyValueStore = keyValueStore;
    cam.active = 1;
    cam.streamingStatus = kStreamingStatus_Available;
    cam.pending.videoSock = cam.pending.audioSock = -1;
    const char* name = getenv("RHKD_NAME");
    if (name && *name)
        accessory.name = name;
    const char* serial = getenv("RHKD_SERIAL");
    if (serial && *serial)
        accessory.serialNumber = serial;
    HAPLogInfo(&logObject, "rhkd %s, camera accessory \"%s\"", RHKD_VERSION, accessory.name);
}

void AppRelease(void) {
    EndSession("shutdown");
}

// Bump this whenever services or characteristics change, so controllers re-read /accessories.
#define kDBRevision ((uint32_t) 7)
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
    if (found && n == sizeof bytes && HAPReadLittleUInt32(bytes) == kDBRevision)
        return;
    HAPLogInfo(&logObject, "Accessory database revision %lu is new: incrementing configuration number.", (unsigned long) kDBRevision);
    if (HAPAccessoryServerIncrementCN(cam.keyValueStore))
        return;
    HAPWriteLittleUInt32(bytes, kDBRevision);
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

// Runs on the HAP run loop when the camera is turned on or off in Home.
static void OperatingModeChanged(bool homeKitActive) {
    if (!homeKitActive) {
        EndSession("camera turned off in Home");
        recording_camera_disabled();
    }
    // StatusActive mirrors HomeKitCameraActive; StreamingStatus/SetupEndpoints reads change with it.
    MotionChanged(motion_detected(), motion_available());
}

void AppAccessoryServerStart(void) {
    BumpConfigurationNumberIfDBChanged();
    opmode_init(cam.server, cam.keyValueStore, OperatingModeChanged);
    recording_init(cam.server, cam.keyValueStore);
    hds_init(recording_hds_message, recording_hds_closed);
    bitrate_recover();
    HAPAccessoryServerStart(cam.server, &accessory);
    if (motion_start(MotionChanged, 500))
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
