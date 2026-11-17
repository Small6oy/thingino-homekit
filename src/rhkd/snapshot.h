// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RHKD_SNAPSHOT_H
#define RHKD_SNAPSHOT_H
#include <stddef.h>
#include <stdint.h>
// Grabs a JPEG produced after this call from raptor ring @ringName. Returns bytes, 0 on failure.
size_t snapshot_take(const char* ringName, uint8_t* out, size_t cap, unsigned timeoutMs);
#endif
