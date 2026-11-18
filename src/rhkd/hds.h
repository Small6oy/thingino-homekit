// SPDX-License-Identifier: GPL-3.0-or-later
// HomeKit Data Stream (HDS): the Data Stream Transport Management service (0x129) and the encrypted TCP
// transport HomeKit Secure Video uses. Ported from HAP-NodeJS src/lib/datastream/ (Apache-2.0,
// Copyright The HAP-NodeJS contributors).
#ifndef RHKD_HDS_H
#define RHKD_HDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "HAP.h"
#include "opack.h"

// HDS response status (header "status").
enum {
    kHDSStatus_Success = 0,
    kHDSStatus_OutOfMemory = 1,
    kHDSStatus_Timeout = 2,
    kHDSStatus_HeaderError = 3,
    kHDSStatus_PayloadError = 4,
    kHDSStatus_MissingProtocol = 5,
    kHDSStatus_ProtocolSpecificError = 6,
};

typedef struct hds_conn hds_conn_t;

// A request or event that arrived on a ready connection (after hello). Runs on the HDS thread.
// For requests, return true if the handler sent (or will send) a response; false makes HDS answer
// MissingProtocol. @msg is the message dictionary (may be an empty dict).
typedef bool (*hds_message_fn)(hds_conn_t* conn, const char* protocol, const char* topic, bool isRequest, int64_t id,
                               const op_val_t* msg);
// A connection closed (any reason). Runs on the HDS thread.
typedef void (*hds_closed_fn)(hds_conn_t* conn);

// Starts the HDS thread. Call once before the accessory server starts.
void hds_init(hds_message_fn onMessage, hds_closed_fn onClosed);

// Call from the HAP run loop when a HAP session ends: closes the HDS connections it set up.
void hds_session_invalidated(HAPSessionRef* session);

// Sending (thread-safe). @msg is an encoded OPACK dictionary (op_writer_t output). Return 0 on success.
int hds_send_response(hds_conn_t* conn, const char* protocol, const char* topic, int64_t id, int status,
                      const uint8_t* msg, size_t msgLen);
int hds_send_event(hds_conn_t* conn, const char* protocol, const char* topic, const uint8_t* msg, size_t msgLen);
int hds_send_request(hds_conn_t* conn, const char* protocol, const char* topic, int64_t id, const uint8_t* msg,
                     size_t msgLen);
void hds_close(hds_conn_t* conn);

// Largest payload one frame can carry (24-bit length, as HAP-NodeJS limits it).
#define HDS_MAX_PAYLOAD 0xFFFFF

extern const HAPService dataStreamService;

#endif
