// SPDX-License-Identifier: GPL-3.0-or-later
// Per-session bitrate: while a HomeKit stream runs from the sub stream, its encoder bitrate follows what iOS asks for
// (Start and Reconfigure), floored at RHKD_MIN_BITRATE_KBPS (300) and never above the configured value. Restored when
// the session ends, and on the next start if rhkd died mid-session. main is never touched. RHKD_ADAPT_BITRATE=false
// turns it off.
#ifndef RHKD_BITRATE_H
#define RHKD_BITRATE_H

void bitrate_apply(const char* ringName, unsigned maxKbps);
void bitrate_restore(void);
void bitrate_recover(void); // call once at startup

#endif
