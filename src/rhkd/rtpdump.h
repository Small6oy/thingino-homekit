// SPDX-License-Identifier: GPL-3.0-or-later
// Full decode of a SelectedRTPStreamConfiguration write (session, video codec/attributes/RTP, audio
// codec/RTP, comfort noise) for the log, as the commercial ADK prints it. Logged at debug level; at info level
// when the write has TLV types outside the known layout, so new iOS fields show up without debug logging.
#ifndef RHKD_RTPDUMP_H
#define RHKD_RTPDUMP_H

#include <stddef.h>
#include <stdint.h>

#include "HAP.h"

void rtpdump_selected(int slot, const uint8_t* tlv, size_t len);

#endif
