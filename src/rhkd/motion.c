// SPDX-License-Identifier: GPL-3.0-or-later
#include "motion.h"
#include "rss_ipc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "HAP.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "Motion" };

#define RMD_SOCK "/var/run/rss/rmd.sock"
#define RVD_SOCK "/var/run/rss/rvd.sock"

static atomic_bool gMotion, gAvailable;
static motion_changed_fn gOnChange;
static unsigned gIntervalMs;

typedef struct {
    bool motion, available;
} change_t;

static void Deliver(void* _Nullable context, size_t contextSize) {
    HAPPrecondition(context && contextSize == sizeof(change_t));
    const change_t* c = context;
    if (gOnChange)
        gOnChange(c->motion, c->available);
}

// Fallback: rmd's state ("active"/"cooldown" = motion). Returns -1 if rmd did not answer.
static int PollRmd(void) {
    char resp[512];
    if (rss_ctrl_send_command(RMD_SOCK, "{\"cmd\":\"status\"}", resp, sizeof resp, 500) <= 0)
        return -1;
    const char* st = strstr(resp, "\"state\"");
    if (!st)
        return -1;
    st = strchr(st + 7, '"');
    if (!st)
        return -1;
    return strncmp(st + 1, "idle", 4) != 0;
}

// rvd's raw IVS result: true only on the frames where motion was seen. -1: rvd did not answer;
// -2: motion detection is off (raptor.conf [motion] enabled), so the sensor can never fire.
static int PollRvd(void) {
    char resp[512];
    if (rss_ctrl_send_command(RVD_SOCK, "{\"cmd\":\"ivs-status\"}", resp, sizeof resp, 500) <= 0)
        return -1;
    if (strstr(resp, "\"active\":false"))
        return -2;
    return strstr(resp, "\"motion\":true") != NULL;
}

static uint64_t NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned EnvUint(const char* name, unsigned def) {
    const char* v = getenv(name);
    return v && *v ? (unsigned) strtoul(v, NULL, 10) : def;
}

// Source: rvd's raw IVS flag (blips of one or two samples). rmd's state is not used for the sensor any more:
// its post-roll (record_post_sec, 30 s by default) is meant for recording and kept the sensor "detected" for
// most of the time on a busy scene. Debounce here instead:
//   on:  RHKD_MOTION_MIN_HITS raw hits (default 2) within RHKD_MOTION_WINDOW_MS (default 3000)
//   off: no raw hit for RHKD_MOTION_HOLD_SEC (default 10)
// rmd is only polled as a fallback source when rvd does not answer.
static void* Thread(void* arg HAP_UNUSED) {
    const unsigned minHits = EnvUint("RHKD_MOTION_MIN_HITS", 2);
    const unsigned windowMs = EnvUint("RHKD_MOTION_WINDOW_MS", 3000);
    const unsigned holdMs = EnvUint("RHKD_MOTION_HOLD_SEC", 10) * 1000;
    HAPLogInfo(&logObject, "motion: on after %u hits in %u ms, off after %u s quiet", minHits, windowMs, holdMs / 1000);

    bool reported = false, lastAvail = false, first = true;
    uint64_t hits[16] = { 0 };
    unsigned nHits = 0;
    uint64_t lastHit = 0;
    const char* lastSource = "";
    for (;;) {
        const char* source = "rvd";
        int m = PollRvd();
        if (m == -2) {
            // Report the sensor as inactive (StatusActive false) instead of a sensor that never triggers.
            source = "off";
            m = -1;
        } else if (m < 0) {
            source = "rmd";
            m = PollRmd();
        }
        bool avail = m >= 0;
        if (strcmp(source, "off") == 0 && strcmp(lastSource, "off") != 0) {
            HAPLogInfo(&logObject, "motion detection is off in raptor.conf ([motion] enabled = true to use the sensor)");
            lastSource = "off";
        }
        uint64_t now = NowMs();
        if (avail && strcmp(source, lastSource) != 0) {
            HAPLogInfo(&logObject, "motion source: %s", source);
            lastSource = source;
        }
        if (m > 0) {
            lastHit = now;
            if (nHits == HAPArrayCount(hits)) {
                memmove(hits, hits + 1, sizeof hits - sizeof hits[0]);
                nHits--;
            }
            hits[nHits++] = now;
        }
        while (nHits && now - hits[0] > windowMs) {
            memmove(hits, hits + 1, (nHits - 1) * sizeof hits[0]);
            nHits--;
        }
        bool motion = reported;
        if (!reported && nHits >= minHits)
            motion = true;
        else if (reported && (!avail || now - lastHit >= holdMs))
            motion = false;

        if (first || motion != reported || avail != lastAvail) {
            atomic_store(&gMotion, motion);
            atomic_store(&gAvailable, avail);
            if (!first || motion || !avail)
                HAPLogInfo(&logObject, "motion %s%s", motion ? "detected" : "cleared", avail ? "" : " (no motion source)");
            change_t c = { motion, avail };
            if (HAPPlatformRunLoopScheduleCallback(Deliver, &c, sizeof c))
                HAPLogError(&logObject, "could not schedule motion update");
            reported = motion;
            lastAvail = avail;
            first = false;
        }
        struct timespec ts = { gIntervalMs / 1000, (long) (gIntervalMs % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int motion_start(motion_changed_fn onChange, unsigned intervalMs) {
    gOnChange = onChange;
    gIntervalMs = intervalMs ? intervalMs : 500;
    pthread_t t;
    if (pthread_create(&t, NULL, Thread, NULL))
        return -1;
    pthread_detach(t);
    return 0;
}

bool motion_detected(void) {
    return atomic_load(&gMotion);
}

bool motion_available(void) {
    return atomic_load(&gAvailable);
}
