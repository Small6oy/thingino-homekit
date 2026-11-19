// SPDX-License-Identifier: GPL-3.0-or-later
// Per-session bitrate for the HomeKit source streams (see bitrate.h).
#include "bitrate.h"
#include "rss_ipc.h"
#include "stream.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "HAP.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "Bitrate" };

#define RVD_SOCK "/var/run/rss/rvd.sock"
#define SAVE_FILE "/run/rhkd-bitrate" // per lowered channel: "<channel> <orig bps> <section> <temporary bps>..."
#define RAPTOR_CONF "/etc/raptor.conf"  // RHKD_RAPTOR_CONF overrides (tests)
#define RAPTORCTL "/usr/bin/raptorctl"
#define MAX_APPLIED 8

// One entry per ring rhkd may lower ("sub", "aux").
// rvd's set-bitrate also stores the value in its in-memory config, so a `raptorctl config save` (e.g. from the web UI)
// during a live view writes the temporary bitrate into raptor.conf, and the next restart starts from it. rhkd remembers
// the temporary values it set and, after restoring the encoder, puts the configured value back in the file if the file
// holds one of them. A value the owner chose (not one of ours) is left alone.
static struct {
    const char* ring;
    const char* section;  // raptor.conf section of the ring's stream
    int owner;            // session slot that set it, -1 if untouched
    int chn;              // rvd channel (the ring's stream_id)
    unsigned origBps;     // bitrate before we touched it
    unsigned curBps;      // what we last set
    unsigned applied[MAX_APPLIED]; // temporary values set this session (oldest replaced when full)
    unsigned nApplied;
} gRings[] = { { .ring = "sub", .section = "stream1", .owner = -1, .chn = -1 },
               { .ring = "aux", .section = "stream2", .owner = -1, .chn = -1 } };

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
        fprintf(f, "%d %u %s", gRings[i].chn, gRings[i].origBps, gRings[i].section);
        for (unsigned k = 0; k < gRings[i].nApplied; k++)
            fprintf(f, " %u", gRings[i].applied[k]);
        fputc('\n', f);
    }
    if (f)
        fclose(f);
    else
        unlink(SAVE_FILE);
}

static void NoteApplied(size_t i, unsigned bps) {
    for (unsigned k = 0; k < gRings[i].nApplied; k++)
        if (gRings[i].applied[k] == bps)
            return;
    if (gRings[i].nApplied < MAX_APPLIED) {
        gRings[i].applied[gRings[i].nApplied++] = bps;
    } else {
        memmove(gRings[i].applied, gRings[i].applied + 1, (MAX_APPLIED - 1) * sizeof gRings[i].applied[0]);
        gRings[i].applied[MAX_APPLIED - 1] = bps;
    }
}

static const char* ConfPath(void) {
    const char* p = getenv("RHKD_RAPTOR_CONF");
    return p && *p ? p : RAPTOR_CONF;
}

// The last `bitrate = N` in [section] of raptor.conf (as raptor's own parser, later keys win).
static bool FileBitrate(const char* section, unsigned* bps) {
    FILE* f = fopen(ConfPath(), "r");
    if (!f)
        return false;
    char line[256];
    bool inSect = false, found = false;
    size_t slen = strlen(section);
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (isspace((unsigned char) *p))
            p++;
        if (*p == '[') {
            inSect = strncmp(p + 1, section, slen) == 0 && p[1 + slen] == ']';
            continue;
        }
        if (!inSect || strncmp(p, "bitrate", 7) != 0)
            continue;
        p += 7;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p++ != '=')
            continue;
        char* end;
        unsigned long v = strtoul(p, &end, 10);
        if (end != p && v > 0) {
            *bps = (unsigned) v;
            found = true;
        }
    }
    fclose(f);
    return found;
}

static int SetFileBitrate(const char* section, unsigned bps) {
    if (getenv("RHKD_RAPTOR_CONF")) { // tests: no raptorctl, write nothing but report
        HAPLogInfo(&logObject, "(test) would set [%s] bitrate = %u", section, bps);
        return 0;
    }
    char value[16];
    snprintf(value, sizeof value, "%u", bps);
    pid_t pid = fork();
    if (pid == 0) {
        execl(RAPTORCTL, "raptorctl", "config", "set", section, "bitrate", value, (char*) NULL);
        _exit(127);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

// After a session: if raptor.conf holds one of the temporary values, a config save caught it; put @origBps back.
static void RepairFile(const char* section, unsigned origBps, const unsigned* applied, unsigned nApplied) {
    unsigned fileBps;
    if (!section || !FileBitrate(section, &fileBps) || fileBps == origBps)
        return;
    bool ours = false;
    for (unsigned k = 0; k < nApplied; k++)
        ours |= applied[k] == fileBps;
    if (!ours)
        return; // the owner changed it; not ours to undo
    if (SetFileBitrate(section, origBps) == 0)
        HAPLogInfo(&logObject, "raptor.conf [%s] had the live view's temporary bitrate %u (a config save during the "
                   "session); set it back to %u.", section, fileBps, origBps);
    else
        HAPLogError(&logObject, "raptor.conf [%s] has the live view's temporary bitrate %u; could not set it back to %u.",
                    section, fileBps, origBps);
}

void bitrate_recover(void) {
    FILE* f = fopen(SAVE_FILE, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int chn = -1, n = 0;
        unsigned bps = 0;
        char section[32] = "";
        if (sscanf(line, "%d %u %31s%n", &chn, &bps, section, &n) < 2 || chn < 0 || !bps)
            continue;
        if (SetBitrate(chn, bps) == 0)
            HAPLogInfo(&logObject, "Restored channel %d to %u bit/s (left lowered by a previous run).", chn, bps);
        else
            HAPLogError(&logObject, "Could not restore channel %d to %u bit/s.", chn, bps);
        unsigned applied[MAX_APPLIED], nApplied = 0;
        for (char* p = line + n; nApplied < MAX_APPLIED;) {
            char* end;
            unsigned long v = strtoul(p, &end, 10);
            if (end == p)
                break;
            applied[nApplied++] = (unsigned) v;
            p = end;
        }
        RepairFile(section[0] ? section : NULL, bps, applied, nApplied);
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
        gRings[i].nApplied = 0;
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
        if (bps != gRings[i].origBps) {
            NoteApplied(i, bps);
            Save();
        }
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
        RepairFile(gRings[i].section, gRings[i].origBps, gRings[i].applied, gRings[i].nApplied);
        gRings[i].nApplied = 0;
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
