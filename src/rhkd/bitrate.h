// SPDX-License-Identifier: GPL-3.0-or-later
// Per-session bitrate: while a HomeKit stream runs from the sub or aux stream, that encoder's bitrate follows what iOS
// asks for (Start and Reconfigure), floored at RHKD_MIN_BITRATE_KBPS (300) and never above the configured value. The
// first session on a ring owns its bitrate; a second session reading the same ring leaves it alone. Restored when the
// owner's session ends, and on the next start if rhkd died mid-session. main is never touched (it feeds recordings).
// rvd keeps set-bitrate values in its in-memory config, so a `config save` during a session would store the temporary
// bitrate in raptor.conf; on release (and recovery) rhkd puts the configured value back if the file holds one of its
// temporary values. RHKD_ADAPT_BITRATE=false turns it off.
#ifndef RHKD_BITRATE_H
#define RHKD_BITRATE_H

void bitrate_apply(int owner, const char* ringName, unsigned maxKbps);
void bitrate_release(int owner); // restores whatever this owner lowered
void bitrate_recover(void);      // call once at startup
unsigned bitrate_configured_kbps(const char* ringName); // the ring's encoder bitrate before rhkd touched it; 0 if unknown

#endif
