// SPDX-License-Identifier: GPL-3.0-or-later
// Camera Operating Mode service (0x21A): HomeKit camera on/off, snapshot policy, indicator LED, night vision.
#ifndef RHKD_OPMODE_H
#define RHKD_OPMODE_H

#include <stdbool.h>

#include "HAP.h"

// Called on the HAP run loop when HomeKitCameraActive changes.
typedef void (*opmode_changed_fn)(bool homeKitCameraActive);

// Loads the saved state from the key-value store and re-applies night vision. Call before the server starts.
void opmode_init(HAPAccessoryServerRef* server, HAPPlatformKeyValueStoreRef kvs, opmode_changed_fn onChange);

// False when the user turned the camera off in Home: no streaming, no snapshots, sensors inactive.
bool opmode_homekit_active(void);

// Snapshot policy. @reason: 0 periodic, 1 event, -1 unknown (as sent in POST /resource).
bool opmode_snapshot_allowed(int reason);

// Live view started / ended: drives the status LED when the operating-mode indicator is on.
void opmode_stream_started(void);
void opmode_stream_ended(void);

extern const HAPService operatingModeService;

#endif
