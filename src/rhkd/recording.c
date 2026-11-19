// SPDX-License-Identifier: GPL-3.0-or-later
//
// Camera Recording Management (HAP 0x204) for HomeKit Secure Video.
//
// Behaviour ported from HAP-NodeJS src/lib/camera/RecordingManagement.ts (Apache-2.0, Copyright The HAP-NodeJS
// contributors): Active / RecordingAudioActive / SelectedCameraRecordingConfiguration are stored; a controller
// asks for recordings with the HDS request dataSend/open {target "controller", type "ipcamera.recording",
// streamId, reason}, which is refused unless recording is active, the camera is on in Home, no other recording
// runs and a configuration was selected.
//
// The supported configurations are the Aqara G100's (a certified HKSV camera), byte for byte from its
// accessory database (research/thingino/aqara-g100/tlv-decoded.txt): prebuffer 4 s, motion trigger, fMP4 with
// 4 s fragments; H.264 Main, levels 3.1/4, 1920x1080 and 1280x720 at 30 and 15 fps; AAC-LC mono 16 kHz.
// Characteristic permissions also follow the G100 (Active and RecordingAudioActive need timed writes).
//
// Media comes from recorder.c (prebuffer + fMP4 over dataSend/data), started on an accepted dataSend/open.

#include "recording.h"

#include <pthread.h>
#include <string.h>

#include "HAPUUID.h"
#include "opmode.h"
#include "recorder.h"
#include "tlvb.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "Recording" };

// HDS dataSend close/error reasons (HAP-NodeJS HDSProtocolSpecificErrorReason).
enum {
    kReason_Normal = 0,
    kReason_NotAllowed = 1,
    kReason_Busy = 2,
    kReason_Cancelled = 3,
    kReason_Unsupported = 4,
    kReason_UnexpectedFailure = 5,
    kReason_Timeout = 6,
    kReason_BadData = 7,
    kReason_ProtocolError = 8,
    kReason_InvalidConfiguration = 9,
};

// The G100's supported configurations (see the header comment).
static const uint8_t kSupportedCamera[] = { // prebuffer 4000 ms, triggers 0x01 (motion), fMP4 fragments 4000 ms
    0x01, 0x04, 0xA0, 0x0F, 0x00, 0x00, 0x02, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x0B, 0x01, 0x01, 0x00, 0x02, 0x06, 0x01, 0x04, 0xA0, 0x0F, 0x00, 0x00,
};
static const uint8_t kSupportedVideo[] = { // H.264 Main, levels 3.1/4; 1080p30, 720p30, 1080p15, 720p15
    0x01, 0x4A, 0x01, 0x01, 0x00, 0x02, 0x0B, 0x01, 0x01, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x02, 0x01, 0x02,
    0x03, 0x0B, 0x01, 0x02, 0x80, 0x07, 0x02, 0x02, 0x38, 0x04, 0x03, 0x01, 0x1E, 0x00, 0x00,
    0x03, 0x0B, 0x01, 0x02, 0x00, 0x05, 0x02, 0x02, 0xD0, 0x02, 0x03, 0x01, 0x1E, 0x00, 0x00,
    0x03, 0x0B, 0x01, 0x02, 0x80, 0x07, 0x02, 0x02, 0x38, 0x04, 0x03, 0x01, 0x0F, 0x00, 0x00,
    0x03, 0x0B, 0x01, 0x02, 0x00, 0x05, 0x02, 0x02, 0xD0, 0x02, 0x03, 0x01, 0x0F,
};
static const uint8_t kSupportedAudio[] = { // AAC-LC, mono, variable bitrate, 16 kHz
    0x01, 0x0E, 0x01, 0x01, 0x00, 0x02, 0x09, 0x01, 0x01, 0x01, 0x02, 0x01, 0x00, 0x03, 0x01, 0x01,
};

#define kKVSDomain  ((HAPPlatformKeyValueStoreDomain) 0x00)
#define kKVSKey     ((HAPPlatformKeyValueStoreKey) 0x03)
#define kKVSVersion 1

static struct {
    HAPAccessoryServerRef* server;
    HAPPlatformKeyValueStoreRef kvs;
    pthread_mutex_t lock; // guards the fields below (HAP run loop writes, HDS thread reads)
    uint8_t active, audioActive;
    uint8_t selected[256];
    size_t selectedLen;
    uint32_t prebufferMs, fragmentMs, iframeMs, audioKbps; // from the selected configuration
} rec = { .lock = PTHREAD_MUTEX_INITIALIZER };

// ---------------------------------------------------------------------------------------------------------------------
// Persistence

static void Save(void) {
    uint8_t b[3 + sizeof rec.selected];
    b[0] = kKVSVersion;
    b[1] = rec.active;
    b[2] = rec.audioActive;
    memcpy(b + 3, rec.selected, rec.selectedLen);
    if (HAPPlatformKeyValueStoreSet(rec.kvs, kKVSDomain, kKVSKey, b, 3 + rec.selectedLen))
        HAPLogError(&logObject, "could not save the recording state");
}

static void Load(void) {
    uint8_t b[3 + sizeof rec.selected];
    size_t n = 0;
    bool found = false;
    if (HAPPlatformKeyValueStoreGet(rec.kvs, kKVSDomain, kKVSKey, b, sizeof b, &n, &found) || !found || n < 3 ||
        b[0] != kKVSVersion)
        return;
    rec.active = b[1] ? 1 : 0;
    rec.audioActive = b[2] ? 1 : 0;
    rec.selectedLen = n - 3;
    memcpy(rec.selected, b + 3, rec.selectedLen);
}

// ---------------------------------------------------------------------------------------------------------------------
// Selected configuration: parse for logging (and, later, for the producer).

static uint32_t U32(const uint8_t* p, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n && i < 4; i++)
        v |= (uint32_t) p[i] << (8 * i);
    return v;
}

static void LogSelected(const uint8_t* t, size_t len) {
    uint8_t recCfg[96], video[96], audio[64], x[64], y[32], z[16];
    size_t rl, vl, al, xl, yl, zl;
    uint32_t prebuffer = 0, triggers = 0, fragment = 0, w = 0, h = 0, fps = 0, profile = 0, level = 0, vbr = 0,
             iframe = 0, acodec = 0, ch = 0, sr = 0, abr = 0;
    if (tlv_get(t, len, 0x01, recCfg, sizeof recCfg, &rl)) {
        if (tlv_get(recCfg, rl, 0x01, x, sizeof x, &xl))
            prebuffer = U32(x, xl);
        if (tlv_get(recCfg, rl, 0x02, x, sizeof x, &xl))
            triggers = U32(x, xl);
        if (tlv_get(recCfg, rl, 0x03, x, sizeof x, &xl) && tlv_get(x, xl, 0x02, y, sizeof y, &yl) &&
            tlv_get(y, yl, 0x01, z, sizeof z, &zl))
            fragment = U32(z, zl);
    }
    if (tlv_get(t, len, 0x02, video, sizeof video, &vl)) {
        if (tlv_get(video, vl, 0x02, x, sizeof x, &xl)) {
            if (tlv_get(x, xl, 0x01, y, sizeof y, &yl))
                profile = U32(y, yl);
            if (tlv_get(x, xl, 0x02, y, sizeof y, &yl))
                level = U32(y, yl);
            if (tlv_get(x, xl, 0x03, y, sizeof y, &yl))
                vbr = U32(y, yl);
            if (tlv_get(x, xl, 0x04, y, sizeof y, &yl))
                iframe = U32(y, yl);
        }
        if (tlv_get(video, vl, 0x03, x, sizeof x, &xl)) {
            if (tlv_get(x, xl, 0x01, y, sizeof y, &yl))
                w = U32(y, yl);
            if (tlv_get(x, xl, 0x02, y, sizeof y, &yl))
                h = U32(y, yl);
            if (tlv_get(x, xl, 0x03, y, sizeof y, &yl))
                fps = U32(y, yl);
        }
    }
    if (tlv_get(t, len, 0x03, audio, sizeof audio, &al)) {
        if (tlv_get(audio, al, 0x01, x, sizeof x, &xl))
            acodec = U32(x, xl);
        if (tlv_get(audio, al, 0x02, x, sizeof x, &xl)) {
            if (tlv_get(x, xl, 0x01, y, sizeof y, &yl))
                ch = U32(y, yl);
            if (tlv_get(x, xl, 0x03, y, sizeof y, &yl))
                sr = U32(y, yl);
            if (tlv_get(x, xl, 0x04, y, sizeof y, &yl))
                abr = U32(y, yl);
        }
    }
    rec.prebufferMs = prebuffer;
    rec.fragmentMs = fragment;
    rec.iframeMs = iframe;
    rec.audioKbps = abr;
    HAPLogInfo(&logObject,
               "Selected recording: prebuffer %u ms, triggers 0x%x, fragment %u ms; video %ux%u@%u profile %u level %u "
               "%u kbit/s I-frame %u ms; audio codec %u, %u ch, rate idx %u, %u kbit/s.",
               prebuffer, triggers, fragment, w, h, fps, profile, level, vbr, iframe, acodec, ch, sr, abr);
}

// ---------------------------------------------------------------------------------------------------------------------
// HDS dataSend (HDS thread)

static void SendOpenError(hds_conn_t* c, int64_t id, int reason) {
    uint8_t b[32];
    op_writer_t w;
    op_w_init(&w, b, sizeof b);
    op_w_dict(&w, 1);
    op_w_str(&w, "status");
    op_w_int(&w, reason);
    (void) hds_send_response(c, "dataSend", "open", id, kHDSStatus_ProtocolSpecificError, b, w.len);
}

static void HandleOpen(hds_conn_t* c, int64_t id, const op_val_t* msg) {
    char target[32] = "", type[48] = "", reason[32] = "";
    op_str(op_get(msg, "target"), target, sizeof target);
    op_str(op_get(msg, "type"), type, sizeof type);
    op_str(op_get(msg, "reason"), reason, sizeof reason);
    const op_val_t* sid = op_get(msg, "streamId");
    int64_t streamId = sid && sid->type == OP_INT ? sid->i : -1;
    HAPLogInfo(&logObject, "dataSend/open: target %s, type %s, streamId %lld, reason %s.", target, type,
               (long long) streamId, reason);

    if (strcmp(target, "controller") || strcmp(type, "ipcamera.recording")) {
        SendOpenError(c, id, kReason_UnexpectedFailure);
        return;
    }
    pthread_mutex_lock(&rec.lock);
    int refuse = !rec.active                ? kReason_NotAllowed
                 : !opmode_homekit_active() ? kReason_NotAllowed
                 : recorder_busy()          ? kReason_Busy
                 : !rec.selectedLen         ? kReason_InvalidConfiguration
                                            : -1;
    pthread_mutex_unlock(&rec.lock);
    if (refuse >= 0) {
        HAPLogInfo(&logObject, "dataSend/open refused (reason %d).", refuse);
        SendOpenError(c, id, refuse);
        return;
    }

    // Accepted: {status: 0}, as HAP-NodeJS CameraRecordingStream does before streaming.
    uint8_t b[16];
    op_writer_t w;
    op_w_init(&w, b, sizeof b);
    op_w_dict(&w, 1);
    op_w_str(&w, "status");
    op_w_int(&w, kHDSStatus_Success);
    (void) hds_send_response(c, "dataSend", "open", id, kHDSStatus_Success, b, w.len);

    if (!recorder_start(c, streamId)) {
        HAPLogError(&logObject, "recorder not running: closing stream %lld.", (long long) streamId);
        recorder_stop(c, true, kReason_UnexpectedFailure);
    }
}

bool recording_hds_message(hds_conn_t* c, const char* protocol, const char* topic, bool isRequest, int64_t id,
                           const op_val_t* msg) {
    if (strcmp(protocol, "dataSend"))
        return false;
    if (isRequest && !strcmp(topic, "open")) {
        HandleOpen(c, id, msg);
        return true;
    }
    if (!isRequest && (!strcmp(topic, "close") || !strcmp(topic, "ack"))) {
        const op_val_t* sid = op_get(msg, "streamId");
        const op_val_t* reason = op_get(msg, "reason");
        HAPLogInfo(&logObject, "dataSend/%s: streamId %lld, reason %lld.", topic,
                   sid && sid->type == OP_INT ? (long long) sid->i : -1LL,
                   reason && reason->type == OP_INT ? (long long) reason->i : -1LL);
        recorder_stop(c, false, 0);
        return true;
    }
    return false;
}

void recording_hds_closed(hds_conn_t* c) {
    recorder_stop(c, false, 0);
}

void recording_camera_disabled(void) {
    recorder_stop(NULL, true, kReason_NotAllowed);
}

// ---------------------------------------------------------------------------------------------------------------------
// Characteristics (HAP run loop)

static HAPError EmitTLV(HAPTLVWriterRef* w, const uint8_t* buf, size_t len) {
    size_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t t = buf[pos], l = buf[pos + 1];
        if (pos + 2u + l > len)
            return kHAPError_Unknown;
        HAPError err = HAPTLVWriterAppend(w, &(const HAPTLV) { .type = t, .value = { .bytes = &buf[pos + 2], .numBytes = l } });
        if (err)
            return err;
        pos += 2u + l;
    }
    return kHAPError_None;
}

#define CONST_TLV_READ(name, data)                                                                                  \
    static HAPError name(HAPAccessoryServerRef* server HAP_UNUSED,                                                  \
                         const HAPTLV8CharacteristicReadRequest* request HAP_UNUSED, HAPTLVWriterRef* w,           \
                         void* _Nullable context HAP_UNUSED) {                                                      \
        return EmitTLV(w, data, sizeof data);                                                                       \
    }
CONST_TLV_READ(HandleSupportedCameraRead, kSupportedCamera)
CONST_TLV_READ(HandleSupportedVideoRead, kSupportedVideo)
CONST_TLV_READ(HandleSupportedAudioRead, kSupportedAudio)

static HAPError HandleSelectedRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request HAP_UNUSED,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    // Empty until a controller selects one (the G100 also reads back empty).
    pthread_mutex_lock(&rec.lock);
    HAPError err = EmitTLV(w, rec.selected, rec.selectedLen);
    pthread_mutex_unlock(&rec.lock);
    return err;
}

static HAPError HandleSelectedWrite(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicWriteRequest* request,
        HAPTLVReaderRef* r,
        void* _Nullable context HAP_UNUSED) {
    uint8_t buf[sizeof rec.selected];
    tlvb_t b;
    tlvb_init(&b, buf, sizeof buf);
    for (;;) {
        bool found;
        HAPTLV tlv;
        if (HAPTLVReaderGetNext(r, &found, &tlv))
            return kHAPError_InvalidData;
        if (!found)
            break;
        tlvb_put(&b, tlv.type, tlv.value.bytes, tlv.value.numBytes);
    }
    if (b.overflow || !b.len)
        return kHAPError_InvalidData;
    LogSelected(buf, b.len);
    pthread_mutex_lock(&rec.lock);
    bool changed = b.len != rec.selectedLen || memcmp(buf, rec.selected, b.len);
    memcpy(rec.selected, buf, b.len);
    rec.selectedLen = b.len;
    pthread_mutex_unlock(&rec.lock);
    if (changed) {
        Save();
        HAPAccessoryServerRaiseEvent(rec.server, request->characteristic, request->service, request->accessory);
    }
    return kHAPError_None;
}

static HAPError HandleActiveRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPUInt8CharacteristicReadRequest* request HAP_UNUSED,
        uint8_t* value,
        void* _Nullable context HAP_UNUSED) {
    *value = rec.active;
    return kHAPError_None;
}

static HAPError HandleActiveWrite(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPUInt8CharacteristicWriteRequest* request,
        uint8_t value,
        void* _Nullable context HAP_UNUSED) {
    value = value ? 1 : 0;
    HAPLogInfo(&logObject, "Recording Active <- %u", value);
    if (rec.active == value)
        return kHAPError_None;
    pthread_mutex_lock(&rec.lock);
    rec.active = value;
    pthread_mutex_unlock(&rec.lock);
    recorder_set_active(value, rec.prebufferMs, rec.fragmentMs, rec.iframeMs);
    Save();
    HAPAccessoryServerRaiseEvent(rec.server, request->characteristic, request->service, request->accessory);
    return kHAPError_None;
}

static HAPError HandleAudioActiveRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPUInt8CharacteristicReadRequest* request HAP_UNUSED,
        uint8_t* value,
        void* _Nullable context HAP_UNUSED) {
    *value = rec.audioActive;
    return kHAPError_None;
}

static HAPError HandleAudioActiveWrite(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPUInt8CharacteristicWriteRequest* request,
        uint8_t value,
        void* _Nullable context HAP_UNUSED) {
    value = value ? 1 : 0;
    HAPLogInfo(&logObject, "RecordingAudioActive <- %u", value);
    if (rec.audioActive == value)
        return kHAPError_None;
    rec.audioActive = value;
    recorder_set_audio(value, rec.audioKbps * 1000);
    Save();
    HAPAccessoryServerRaiseEvent(rec.server, request->characteristic, request->service, request->accessory);
    return kHAPError_None;
}

void recording_init(HAPAccessoryServerRef* server, HAPPlatformKeyValueStoreRef kvs) {
    rec.server = server;
    rec.kvs = kvs;
    Load();
    HAPLogInfo(&logObject, "Recording %s, audio %s, %s.", rec.active ? "active" : "inactive",
               rec.audioActive ? "on" : "off", rec.selectedLen ? "configuration selected" : "no configuration selected");
    if (rec.selectedLen)
        LogSelected(rec.selected, rec.selectedLen);
    recorder_set_audio(rec.audioActive, rec.audioKbps * 1000);
    if (rec.active)
        recorder_set_active(true, rec.prebufferMs, rec.fragmentMs, rec.iframeMs);
}

// ---------------------------------------------------------------------------------------------------------------------
// Attribute database

static const HAPUUID kServiceType_CameraRecordingManagement = HAPUUIDCreateAppleDefined(0x204);
static const HAPUUID kCharType_SupportedCameraRecordingConfiguration = HAPUUIDCreateAppleDefined(0x205);
static const HAPUUID kCharType_SupportedVideoRecordingConfiguration = HAPUUIDCreateAppleDefined(0x206);
static const HAPUUID kCharType_SupportedAudioRecordingConfiguration = HAPUUIDCreateAppleDefined(0x207);
static const HAPUUID kCharType_SelectedCameraRecordingConfiguration = HAPUUIDCreateAppleDefined(0x209);
static const HAPUUID kCharType_RecordingAudioActive = HAPUUIDCreateAppleDefined(0x226);

#define kIID_Recording            ((uint64_t) 0x0080)
#define kIID_RecordingActive      ((uint64_t) 0x0081)
#define kIID_RecordingSupported   ((uint64_t) 0x0082)
#define kIID_RecordingVideo       ((uint64_t) 0x0083)
#define kIID_RecordingAudio       ((uint64_t) 0x0084)
#define kIID_RecordingSelected    ((uint64_t) 0x0085)
#define kIID_RecordingAudioActive ((uint64_t) 0x0086)

#define PROPS(r, w, ev, tw) \
    { .readable = r, \
      .writable = w, \
      .supportsEventNotification = ev, \
      .hidden = false, \
      .requiresTimedWrite = tw, \
      .supportsAuthorizationData = false, \
      .ip = { .controlPoint = false, .supportsWriteResponse = false }, \
      .ble = { .supportsBroadcastNotification = false, \
               .supportsDisconnectedNotification = false, \
               .readableWithoutSecurity = false, \
               .writableWithoutSecurity = false } }

static const HAPUInt8Characteristic activeCharacteristic = {
    .format = kHAPCharacteristicFormat_UInt8,
    .iid = kIID_RecordingActive,
    .characteristicType = &kHAPCharacteristicType_Active,
    .debugDescription = kHAPCharacteristicDebugDescription_Active,
    .properties = PROPS(true, true, true, true),
    .units = kHAPCharacteristicUnits_None,
    .constraints = { .minimumValue = 0, .maximumValue = 1, .stepValue = 1 },
    .callbacks = { .handleRead = HandleActiveRead, .handleWrite = HandleActiveWrite },
};

#define TLV_CHAR(var, iidv, type, desc, w, rd, wr)                                                                  \
    static const HAPTLV8Characteristic var = {                                                                      \
        .format = kHAPCharacteristicFormat_TLV8,                                                                    \
        .iid = iidv,                                                                                                \
        .characteristicType = &type,                                                                                \
        .debugDescription = desc,                                                                                   \
        .properties = PROPS(true, w, true, false),                                                                  \
        .callbacks = { .handleRead = rd, .handleWrite = wr },                                                       \
    };
TLV_CHAR(supportedCameraCharacteristic, kIID_RecordingSupported, kCharType_SupportedCameraRecordingConfiguration,
         "supported-camera-recording-configuration", false, HandleSupportedCameraRead, NULL)
TLV_CHAR(supportedVideoCharacteristic, kIID_RecordingVideo, kCharType_SupportedVideoRecordingConfiguration,
         "supported-video-recording-configuration", false, HandleSupportedVideoRead, NULL)
TLV_CHAR(supportedAudioCharacteristic, kIID_RecordingAudio, kCharType_SupportedAudioRecordingConfiguration,
         "supported-audio-recording-configuration", false, HandleSupportedAudioRead, NULL)
TLV_CHAR(selectedCharacteristic, kIID_RecordingSelected, kCharType_SelectedCameraRecordingConfiguration,
         "selected-camera-recording-configuration", true, HandleSelectedRead, HandleSelectedWrite)

static const HAPUInt8Characteristic audioActiveCharacteristic = {
    .format = kHAPCharacteristicFormat_UInt8,
    .iid = kIID_RecordingAudioActive,
    .characteristicType = &kCharType_RecordingAudioActive,
    .debugDescription = "recording-audio-active",
    .properties = PROPS(true, true, true, true),
    .units = kHAPCharacteristicUnits_None,
    .constraints = { .minimumValue = 0, .maximumValue = 1, .stepValue = 1 },
    .callbacks = { .handleRead = HandleAudioActiveRead, .handleWrite = HandleAudioActiveWrite },
};

// Linked to the Data Stream Transport Management (0x70) and Motion Sensor (0x50) services, as on the G100.
static const uint16_t kLinked[] = { 0x0070, 0x0050, 0 };

const HAPService recordingService = {
    .iid = kIID_Recording,
    .serviceType = &kServiceType_CameraRecordingManagement,
    .debugDescription = "camera-recording-management",
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = kLinked,
    .characteristics = (const HAPCharacteristic* const[]) { &activeCharacteristic,
                                                            &supportedCameraCharacteristic,
                                                            &supportedVideoCharacteristic,
                                                            &supportedAudioCharacteristic,
                                                            &selectedCharacteristic,
                                                            &audioActiveCharacteristic,
                                                            NULL },
};
