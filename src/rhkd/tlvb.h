// Minimal TLV8 builder/parser for HAP camera characteristics.
// Lists follow HAP-NodeJS tlv.encode(): each list element is separated by a
// zero-length 0x00 delimiter item.
// SPDX-License-Identifier: Apache-2.0
#ifndef RHKD_TLVB_H
#define RHKD_TLVB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t* buf;
    size_t len;
    size_t cap;
    bool overflow;
} tlvb_t;

void tlvb_init(tlvb_t* b, uint8_t* buf, size_t cap);
void tlvb_put(tlvb_t* b, uint8_t type, const void* data, size_t n); // fragments values > 255 bytes
void tlvb_u8(tlvb_t* b, uint8_t type, uint8_t v);
void tlvb_u16(tlvb_t* b, uint8_t type, uint16_t v); // little endian
void tlvb_u32(tlvb_t* b, uint8_t type, uint32_t v); // little endian
void tlvb_sep(tlvb_t* b);                           // 00 00 list delimiter

// Finds the first item of @type (merging consecutive fragments) and copies its value.
// Returns true if found and it fit into @out.
bool tlv_get(const uint8_t* buf, size_t len, uint8_t type, void* out, size_t cap, size_t* outLen);
bool tlv_get_u8(const uint8_t* buf, size_t len, uint8_t type, uint8_t* v);
bool tlv_get_u16(const uint8_t* buf, size_t len, uint8_t type, uint16_t* v);
bool tlv_get_u32(const uint8_t* buf, size_t len, uint8_t type, uint32_t* v);

// Iterates top-level items (fragments merged). Returns false at end / on error.
// *pos is the cursor (start at 0). @val must hold up to @cap bytes.
bool tlv_next(const uint8_t* buf, size_t len, size_t* pos, uint8_t* type, uint8_t* val, size_t cap, size_t* valLen);
#endif
