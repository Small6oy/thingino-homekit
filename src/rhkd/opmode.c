// SPDX-License-Identifier: GPL-3.0-or-later
//
// Camera Operating Mode service (HAP 0x21A).
//
// Behaviour follows HAP-NodeJS (src/lib/camera/RecordingManagement.ts, src/lib/controller/CameraController.ts,
// Apache-2.0, Copyright The HAP-NodeJS contributors): HomeKitCameraActive gates streaming and snapshots and is
// mirrored to the sensors' StatusActive; Event/PeriodicSnapshotsActive gate snapshots by request reason.
// The characteristic set and permissions match the Aqara G100 (commercial ADK 6.3), which Home accepts:
// EventSnapshotsActive, PeriodicSnapshotsActive, HomeKitCameraActive, ThirdPartyCameraActive (read-only),
// CameraOperatingModeIndicator and NightVision, none of them timed-write.
//
// Night vision drives raptor's IR-cut daemon (ric): on = "auto", off = "day" (IR LEDs off, filter in).
// The indicator lights the status LED (Thingino's `led` tool) while a live view is running.

#include "opmode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "HAPPlatformTimer.h"
#include "HAPUUID.h"
#include "rss_ipc.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "OperatingMode" };

static const HAPUUID kServiceType_CameraOperatingMode = HAPUUIDCreateAppleDefined(0x21A);
static const HAPUUID kCharType_HomeKitCameraActive = HAPUUIDCreateAppleDefined(0x21B);
static const HAPUUID kCharType_ThirdPartyCameraActive = HAPUUIDCreateAppleDefined(0x21C);
static const HAPUUID kCharType_OperatingModeIndicator = HAPUUIDCreateAppleDefined(0x21D);
static const HAPUUID kCharType_EventSnapshotsActive = HAPUUIDCreateAppleDefined(0x223);
static const HAPUUID kCharType_PeriodicSnapshotsActive = HAPUUIDCreateAppleDefined(0x225);
static const HAPUUID kCharType_NightVision = HAPUUIDCreateAppleDefined(0x11B);

#define RIC_SOCK "/var/run/rss/ric.sock"
#define LED_TOOL "/usr/sbin/led"

// Saved in the ADK key-value store (app domain 0x00, next to the DB revision at key 0x01).
#define kKVSDomain ((HAPPlatformKeyValueStoreDomain) 0x00)
#define kKVSKey    ((HAPPlatformKeyValueStoreKey) 0x02)
#define kKVSVersion 1

static struct {
    HAPAccessoryServerRef* server;
    HAPPlatformKeyValueStoreRef kvs;
    opmode_changed_fn onChange;
    uint8_t homeKitActive, eventSnapshots, periodicSnapshots, indicator;
    bool nightVision;
    bool streaming;
    HAPPlatformTimerRef retryTimer;
    unsigned retries;
} om = { .homeKitActive = 1, .eventSnapshots = 1, .periodicSnapshots = 1, .indicator = 0, .nightVision = true };

extern const HAPAccessory* AppGetAccessoryInfo(void);

// ---------------------------------------------------------------------------------------------------------------------
// Persistence

static void Save(void) {
    uint8_t b[6] = { kKVSVersion, om.homeKitActive, om.eventSnapshots, om.periodicSnapshots, om.indicator,
                     om.nightVision };
    if (HAPPlatformKeyValueStoreSet(om.kvs, kKVSDomain, kKVSKey, b, sizeof b))
        HAPLogError(&logObject, "could not save the operating mode");
}

static void Load(void) {
    uint8_t b[6];
    size_t n = 0;
    bool found = false;
    if (HAPPlatformKeyValueStoreGet(om.kvs, kKVSDomain, kKVSKey, b, sizeof b, &n, &found) || !found ||
        n != sizeof b || b[0] != kKVSVersion)
        return;
    om.homeKitActive = b[1] ? 1 : 0;
    om.eventSnapshots = b[2] ? 1 : 0;
    om.periodicSnapshots = b[3] ? 1 : 0;
    om.indicator = b[4] ? 1 : 0;
    om.nightVision = b[5] != 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Hardware: night vision (ric) and the status LED

// Returns true when ric accepted the mode.
static bool ApplyNightVision(void) {
    char cmd[64], resp[256];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"mode\",\"value\":\"%s\"}", om.nightVision ? "auto" : "day");
    if (rss_ctrl_send_command(RIC_SOCK, cmd, resp, sizeof resp, 1000) <= 0 || !strstr(resp, "\"ok\"")) {
        HAPLogError(&logObject, "ric did not accept night vision %s (is ric running?)", om.nightVision ? "auto" : "day");
        return false;
    }
    HAPLogInfo(&logObject, "Night vision %s: ric mode %s.", om.nightVision ? "on" : "off", om.nightVision ? "auto" : "day");
    return true;
}

// At startup ric may not be up yet; keep trying for about a minute when night vision has to be forced off.
static void RetryNightVision(HAPPlatformTimerRef timer HAP_UNUSED, void* _Nullable context HAP_UNUSED) {
    om.retryTimer = 0;
    if (ApplyNightVision() || ++om.retries >= 12)
        return;
    if (HAPPlatformTimerRegister(&om.retryTimer, HAPPlatformClockGetCurrent() + 5000, RetryNightVision, NULL))
        om.retryTimer = 0;
}

// Runs Thingino's `led <color|off>` without waiting on the run loop (double fork, so no zombie).
static void Led(bool on) {
    const char* color = getenv("RHKD_LED_COLOR");
    if (!color || !*color)
        color = "b";
    if (!strcmp(color, "none") || access(LED_TOOL, X_OK))
        return;
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            execl(LED_TOOL, "led", on ? color : "off", (char*) NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        (void) waitpid(pid, NULL, 0);
}

static void UpdateLed(void) {
    Led(om.indicator && om.streaming && om.homeKitActive);
}

// ---------------------------------------------------------------------------------------------------------------------
// Public API

void opmode_init(HAPAccessoryServerRef* server, HAPPlatformKeyValueStoreRef kvs, opmode_changed_fn onChange) {
    om.server = server;
    om.kvs = kvs;
    om.onChange = onChange;
    Load();
    HAPLogInfo(&logObject, "HomeKit camera %s, event snapshots %s, periodic snapshots %s, indicator %s, night vision %s.",
               om.homeKitActive ? "on" : "off", om.eventSnapshots ? "on" : "off", om.periodicSnapshots ? "on" : "off",
               om.indicator ? "on" : "off", om.nightVision ? "on" : "off");
    // Only force ric when night vision was turned off in Home; otherwise leave raptor.conf's own mode alone.
    if (!om.nightVision && !ApplyNightVision() &&
        HAPPlatformTimerRegister(&om.retryTimer, HAPPlatformClockGetCurrent() + 5000, RetryNightVision, NULL))
        HAPLogError(&logObject, "could not schedule the night vision retry");
}

bool opmode_homekit_active(void) {
    return om.homeKitActive != 0;
}

bool opmode_snapshot_allowed(int reason) {
    if (!om.homeKitActive)
        return false;
    // As HAP-NodeJS: with a snapshot kind disabled, a request must say why, and must not be of that kind.
    if (!om.eventSnapshots && (reason < 0 || reason == 1))
        return false;
    if (!om.periodicSnapshots && (reason < 0 || reason == 0))
        return false;
    return true;
}

void opmode_stream_started(void) {
    om.streaming = true;
    UpdateLed();
}

void opmode_stream_ended(void) {
    if (!om.streaming)
        return;
    om.streaming = false;
    UpdateLed();
}

// ---------------------------------------------------------------------------------------------------------------------
// Characteristic handlers

static HAPError ReadU8(const uint8_t* field, uint8_t* value) {
    *value = *field;
    return kHAPError_None;
}

#define U8_HANDLERS(name, field, after)                                                                        \
    static HAPError Handle##name##Read(                                                                        \
            HAPAccessoryServerRef* server HAP_UNUSED,                                                          \
            const HAPUInt8CharacteristicReadRequest* request HAP_UNUSED,                                       \
            uint8_t* value,                                                                                    \
            void* _Nullable context HAP_UNUSED) {                                                              \
        return ReadU8(&om.field, value);                                                                       \
    }                                                                                                          \
    static HAPError Handle##name##Write(                                                                       \
            HAPAccessoryServerRef* server HAP_UNUSED,                                                          \
            const HAPUInt8CharacteristicWriteRequest* request,                                                 \
            uint8_t value,                                                                                     \
            void* _Nullable context HAP_UNUSED) {                                                              \
        value = value ? 1 : 0;                                                                                 \
        HAPLogInfo(&logObject, #name " <- %u", value);                                                         \
        if (om.field == value)                                                                                 \
            return kHAPError_None;                                                                             \
        om.field = value;                                                                                      \
        Save();                                                                                                \
        after;                                                                                                 \
        HAPAccessoryServerRaiseEvent(om.server, request->characteristic, request->service, request->accessory); \
        return kHAPError_None;                                                                                 \
    }

U8_HANDLERS(HomeKitCameraActive, homeKitActive, {
    UpdateLed();
    if (om.onChange)
        om.onChange(om.homeKitActive != 0);
})
U8_HANDLERS(EventSnapshotsActive, eventSnapshots, {})
U8_HANDLERS(PeriodicSnapshotsActive, periodicSnapshots, {})
U8_HANDLERS(Indicator, indicator, UpdateLed())

static HAPError HandleThirdPartyActiveRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPUInt8CharacteristicReadRequest* request HAP_UNUSED,
        uint8_t* value,
        void* _Nullable context HAP_UNUSED) {
    *value = 1; // raptor's RTSP/WebRTC servers stay available whatever HomeKit does
    return kHAPError_None;
}

static HAPError HandleNightVisionRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPBoolCharacteristicReadRequest* request HAP_UNUSED,
        bool* value,
        void* _Nullable context HAP_UNUSED) {
    *value = om.nightVision;
    return kHAPError_None;
}

static HAPError HandleNightVisionWrite(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPBoolCharacteristicWriteRequest* request,
        bool value,
        void* _Nullable context HAP_UNUSED) {
    HAPLogInfo(&logObject, "NightVision <- %d", value);
    if (om.nightVision == value)
        return kHAPError_None;
    om.nightVision = value;
    if (om.retryTimer) {
        HAPPlatformTimerDeregister(om.retryTimer);
        om.retryTimer = 0;
    }
    if (!ApplyNightVision()) {
        om.nightVision = !value;
        return kHAPError_Unknown; // ric unreachable: tell Home the change did not happen
    }
    Save();
    HAPAccessoryServerRaiseEvent(om.server, request->characteristic, request->service, request->accessory);
    return kHAPError_None;
}

// ---------------------------------------------------------------------------------------------------------------------
// Attribute database

#define kIID_OperatingMode           ((uint64_t) 0x0060)
#define kIID_EventSnapshotsActive    ((uint64_t) 0x0061)
#define kIID_PeriodicSnapshotsActive ((uint64_t) 0x0062)
#define kIID_HomeKitCameraActive     ((uint64_t) 0x0063)
#define kIID_ThirdPartyCameraActive  ((uint64_t) 0x0064)
#define kIID_OperatingModeIndicator  ((uint64_t) 0x0065)
#define kIID_NightVision             ((uint64_t) 0x0066)

#define PROPS(r, w, ev) \
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

#define U8_CHAR(var, iidv, type, desc, w, rd, wr)                                                              \
    static const HAPUInt8Characteristic var = {                                                                \
        .format = kHAPCharacteristicFormat_UInt8,                                                              \
        .iid = iidv,                                                                                           \
        .characteristicType = &type,                                                                           \
        .debugDescription = desc,                                                                              \
        .properties = PROPS(true, w, true),                                                                    \
        .units = kHAPCharacteristicUnits_None,                                                                 \
        .constraints = { .minimumValue = 0, .maximumValue = 1, .stepValue = 1 },                               \
        .callbacks = { .handleRead = rd, .handleWrite = wr },                                                  \
    };

U8_CHAR(eventSnapshotsCharacteristic, kIID_EventSnapshotsActive, kCharType_EventSnapshotsActive,
        "event-snapshots-active", true, HandleEventSnapshotsActiveRead, HandleEventSnapshotsActiveWrite)
U8_CHAR(periodicSnapshotsCharacteristic, kIID_PeriodicSnapshotsActive, kCharType_PeriodicSnapshotsActive,
        "periodic-snapshots-active", true, HandlePeriodicSnapshotsActiveRead, HandlePeriodicSnapshotsActiveWrite)
U8_CHAR(homeKitActiveCharacteristic, kIID_HomeKitCameraActive, kCharType_HomeKitCameraActive,
        "homekit-camera-active", true, HandleHomeKitCameraActiveRead, HandleHomeKitCameraActiveWrite)
U8_CHAR(thirdPartyActiveCharacteristic, kIID_ThirdPartyCameraActive, kCharType_ThirdPartyCameraActive,
        "third-party-camera-active", false, HandleThirdPartyActiveRead, NULL)
U8_CHAR(indicatorCharacteristic, kIID_OperatingModeIndicator, kCharType_OperatingModeIndicator,
        "camera-operating-mode-indicator", true, HandleIndicatorRead, HandleIndicatorWrite)

static const HAPBoolCharacteristic nightVisionCharacteristic = {
    .format = kHAPCharacteristicFormat_Bool,
    .iid = kIID_NightVision,
    .characteristicType = &kCharType_NightVision,
    .debugDescription = "night-vision",
    .properties = PROPS(true, true, true),
    .callbacks = { .handleRead = HandleNightVisionRead, .handleWrite = HandleNightVisionWrite },
};

const HAPService operatingModeService = {
    .iid = kIID_OperatingMode,
    .serviceType = &kServiceType_CameraOperatingMode,
    .debugDescription = "camera-operating-mode",
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &eventSnapshotsCharacteristic,
                                                            &periodicSnapshotsCharacteristic,
                                                            &homeKitActiveCharacteristic,
                                                            &thirdPartyActiveCharacteristic,
                                                            &indicatorCharacteristic,
                                                            &nightVisionCharacteristic,
                                                            NULL },
};
