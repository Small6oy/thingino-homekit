// SPDX-License-Identifier: GPL-3.0-or-later
// Camera Recording Management service (0x204, HomeKit Secure Video) and its HDS dataSend protocol.
#ifndef RHKD_RECORDING_H
#define RHKD_RECORDING_H

#include <stdbool.h>

#include "HAP.h"
#include "hds.h"

// Loads the saved selection/state. Call before the server starts.
void recording_init(HAPAccessoryServerRef* server, HAPPlatformKeyValueStoreRef kvs);

// HDS callbacks (pass to hds_init). Run on the HDS thread.
bool recording_hds_message(hds_conn_t* conn, const char* protocol, const char* topic, bool isRequest, int64_t id,
                           const op_val_t* msg);
void recording_hds_closed(hds_conn_t* conn);

// HomeKitCameraActive turned off: end any running recording (HAP-NodeJS closes it with NOT_ALLOWED).
void recording_camera_disabled(void);

extern const HAPService recordingService;

#endif
