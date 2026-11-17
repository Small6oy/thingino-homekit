// SPDX-License-Identifier: GPL-3.0-or-later
// Per-session bitrate for the HomeKit source stream (see bitrate.h).
#include "bitrate.h"
#include "rss_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "HAP.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "Bitrate" };

#define RVD_SOCK "/var/run/rss/rvd.sock"
#define SAVE_FILE "/run/rhkd-bitrate" // "<channel> <bps>": the original, while lowered (survives an rhkd crash)

static int gChn = -1;          // channel we lowered, -1 if none
static unsigned gOrigBps;      // its bitrate before we touched it
static unsigned gCurBps;       // what we last set

static bool Enabled(void) {
    const char* v = getenv("RHKD_ADAPT_BITRATE");
    return !v || !*v || strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0;
}

static unsigned MinKbps(void) {
    const char* v = getenv("RHKD_MIN_BITRATE_KBPS");
    int n = v ? atoi(v) : 0;
    return n > 0 ? (unsigned) n : 300;
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

void bitrate_recover(void) {
    FILE* f = fopen(SAVE_FILE, "r");
    if (!f)
        return;
    int chn = -1;
    unsigned bps = 0;
    if (fscanf(f, "%d %u", &chn, &bps) == 2 && chn >= 0 && bps > 0) {
        if (SetBitrate(chn, bps) == 0)
            HAPLogInfo(&logObject, "Restored channel %d to %u bit/s (left lowered by a previous run).", chn, bps);
        else
            HAPLogError(&logObject, "Could not restore channel %d to %u bit/s.", chn, bps);
    }
    fclose(f);
    unlink(SAVE_FILE);
}

void bitrate_apply(const char* ringName, unsigned maxKbps) {
    // Only the sub stream: main feeds recordings, and 2560x1440 at a few hundred kbit/s would look awful anyway.
    if (!Enabled() || !ringName || strcmp(ringName, "sub") != 0 || !maxKbps)
        return;
    int chn = 1;
    if (gChn != chn) {
        bitrate_restore();
        if (GetBitrate(chn, &gOrigBps) != 0) {
            HAPLogError(&logObject, "Cannot read sub bitrate; leaving it alone.");
            return;
        }
        gChn = chn;
        gCurBps = gOrigBps;
        FILE* f = fopen(SAVE_FILE, "w");
        if (f) {
            fprintf(f, "%d %u\n", chn, gOrigBps);
            fclose(f);
        }
    }
    unsigned kbps = maxKbps < MinKbps() ? MinKbps() : maxKbps;
    unsigned bps = kbps * 1000;
    if (bps > gOrigBps)
        bps = gOrigBps; // never above what the owner configured
    if (bps == gCurBps)
        return;
    if (SetBitrate(chn, bps) == 0) {
        HAPLogInfo(&logObject, "sub: %u -> %u bit/s (iOS asks max %u kbit/s, configured %u).", gCurBps, bps, maxKbps,
                   gOrigBps);
        gCurBps = bps;
    } else {
        HAPLogError(&logObject, "set-bitrate on sub failed.");
    }
}

void bitrate_restore(void) {
    if (gChn < 0)
        return;
    if (gCurBps != gOrigBps) {
        if (SetBitrate(gChn, gOrigBps) == 0)
            HAPLogInfo(&logObject, "sub: restored %u bit/s.", gOrigBps);
        else
            HAPLogError(&logObject, "Could not restore sub to %u bit/s.", gOrigBps);
    }
    gChn = -1;
    unlink(SAVE_FILE);
}
