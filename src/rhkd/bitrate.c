// SPDX-License-Identifier: GPL-3.0-or-later
// Per-session bitrate for the HomeKit source streams (see bitrate.h).
#include "bitrate.h"
#include "rss_ipc.h"
#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "HAP.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "Bitrate" };

#define RVD_SOCK "/var/run/rss/rvd.sock"
#define SAVE_FILE "/run/rhkd-bitrate" // "<channel> <bps>" per lowered channel: the originals (survive an rhkd crash)

// One entry per ring rhkd may lower ("sub", "aux").
static struct {
    const char* ring;
    int owner;         // session slot that set it, -1 if untouched
    int chn;           // rvd channel (the ring's stream_id)
    unsigned origBps;  // bitrate before we touched it
    unsigned curBps;   // what we last set
} gRings[] = { { "sub", -1, -1, 0, 0 }, { "aux", -1, -1, 0, 0 } };

static bool Enabled(void) {
    const char* v = getenv("RHKD_ADAPT_BITRATE");
    return !v || !*v || strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0;
}

// Floor for what iOS asks. sub is shared with other readers (Frigate's detect stream), so it keeps
// RHKD_MIN_BITRATE_KBPS (default 300). aux only feeds HomeKit, so it follows iOS down like a certified camera does
// (the Aqara G100 drops to ~132 kbit/s on a Reconfigure to 132; aqara-g100.md), with RHKD_MIN_AUX_BITRATE_KBPS
// (default 100) only as a guard against nonsense values.
static unsigned EnvKbps(const char* name, unsigned dflt) {
    const char* v = getenv(name);
    int n = v ? atoi(v) : 0;
    return n > 0 ? (unsigned) n : dflt;
}

static unsigned MinKbps(const char* ring) {
    if (strcmp(ring, "aux") == 0)
        return EnvKbps("RHKD_MIN_AUX_BITRATE_KBPS", 100);
    return EnvKbps("RHKD_MIN_BITRATE_KBPS", 300);
}

static int GetBitrate(int chn, unsigned* bps) {
    char cmd[64], resp[256];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"get-bitrate\",\"channel\":%d}", chn);
    if (rss_ctrl_send_command(RVD_SOCK, cmd, resp, sizeof resp, 500) <= 0)
        return -1;
    const char* p = strstr(resp, "\"bitrate\":");
    if (!p)
        return -1;
    long v = strtol(p + 10, NULL, 10);
    if (v <= 0)
        return -1;
    *bps = (unsigned) v;
    return 0;
}

static int SetBitrate(int chn, unsigned bps) {
    char cmd[96], resp[256];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"set-bitrate\",\"channel\":%d,\"value\":%u}", chn, bps);
    if (rss_ctrl_send_command(RVD_SOCK, cmd, resp, sizeof resp, 500) <= 0 || !strstr(resp, "\"ok\""))
        return -1;
    return 0;
}

static void Save(void) {
    FILE* f = NULL;
    for (size_t i = 0; i < HAPArrayCount(gRings); i++) {
        if (gRings[i].owner < 0)
            continue;
        if (!f && !(f = fopen(SAVE_FILE, "w")))
            return;
        fprintf(f, "%d %u\n", gRings[i].chn, gRings[i].origBps);
    }
    if (f)
        fclose(f);
    else
        unlink(SAVE_FILE);
}

void bitrate_recover(void) {
    FILE* f = fopen(SAVE_FILE, "r");
    if (!f)
        return;
    int chn = -1;
    unsigned bps = 0;
    while (fscanf(f, "%d %u", &chn, &bps) == 2) {
        if (chn < 0 || !bps)
            continue;
        if (SetBitrate(chn, bps) == 0)
            HAPLogInfo(&logObject, "Restored channel %d to %u bit/s (left lowered by a previous run).", chn, bps);
        else
            HAPLogError(&logObject, "Could not restore channel %d to %u bit/s.", chn, bps);
    }
    fclose(f);
    unlink(SAVE_FILE);
}

void bitrate_apply(int owner, const char* ringName, unsigned maxKbps) {
    if (!Enabled() || !ringName || !maxKbps)
        return;
    size_t i;
    for (i = 0; i < HAPArrayCount(gRings); i++)
        if (strcmp(gRings[i].ring, ringName) == 0)
            break;
    if (i == HAPArrayCount(gRings))
        return; // main: feeds recordings, never touched
    if (gRings[i].owner >= 0 && gRings[i].owner != owner)
        return; // another session owns this encoder's bitrate
    if (gRings[i].owner < 0) {
        stream_ring_info_t info;
        if (!stream_ring_info(ringName, &info) || GetBitrate((int) info.streamId, &gRings[i].origBps) != 0) {
            HAPLogError(&logObject, "Cannot read %s bitrate; leaving it alone.", ringName);
            return;
        }
        gRings[i].owner = owner;
        gRings[i].chn = (int) info.streamId;
        gRings[i].curBps = gRings[i].origBps;
        Save();
    }
    unsigned floorKbps = MinKbps(gRings[i].ring);
    unsigned kbps = maxKbps < floorKbps ? floorKbps : maxKbps;
    unsigned bps = kbps * 1000;
    if (bps > gRings[i].origBps)
        bps = gRings[i].origBps; // never above what the owner configured
    if (bps == gRings[i].curBps)
        return;
    if (SetBitrate(gRings[i].chn, bps) == 0) {
        HAPLogInfo(&logObject, "%s: %u -> %u bit/s (iOS asks max %u kbit/s, configured %u).", ringName,
                   gRings[i].curBps, bps, maxKbps, gRings[i].origBps);
        gRings[i].curBps = bps;
    } else {
        HAPLogError(&logObject, "set-bitrate on %s (channel %d) failed.", ringName, gRings[i].chn);
    }
}

void bitrate_release(int owner) {
    bool changed = false;
    for (size_t i = 0; i < HAPArrayCount(gRings); i++) {
        if (gRings[i].owner != owner)
            continue;
        if (gRings[i].curBps != gRings[i].origBps) {
            if (SetBitrate(gRings[i].chn, gRings[i].origBps) == 0)
                HAPLogInfo(&logObject, "%s: restored %u bit/s.", gRings[i].ring, gRings[i].origBps);
            else
                HAPLogError(&logObject, "Could not restore %s to %u bit/s.", gRings[i].ring, gRings[i].origBps);
        }
        gRings[i].owner = gRings[i].chn = -1;
        changed = true;
    }
    if (changed)
        Save();
}

unsigned bitrate_configured_kbps(const char* ringName) {
    for (size_t i = 0; i < HAPArrayCount(gRings); i++)
        if (strcmp(gRings[i].ring, ringName) == 0 && gRings[i].owner >= 0)
            return gRings[i].origBps / 1000;
    stream_ring_info_t info;
    unsigned bps;
    if (!stream_ring_info(ringName, &info) || GetBitrate((int) info.streamId, &bps) != 0)
        return 0;
    return bps / 1000;
}
