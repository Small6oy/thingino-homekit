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

static atomic_bool gMotion, gAvailable, gClassAvailable;
static atomic_bool gClassMotion[RHKD_MAX_CLASS_SENSORS];
static motion_changed_fn gOnChange;
static motion_class_changed_fn gOnClass;
static const classes_t* gClasses;
static unsigned gIntervalMs;

typedef struct {
    int idx; // -1 = the main sensor
    bool motion, available;
} change_t;

static void Deliver(void* _Nullable context, size_t contextSize) {
    HAPPrecondition(context && contextSize == sizeof(change_t));
    const change_t* c = context;
    if (c->idx < 0) {
        if (gOnChange)
            gOnChange(c->motion, c->available);
    } else if (gOnClass) {
        gOnClass(c->idx, c->motion, c->available);
    }
}

static void Schedule(int idx, bool motion, bool available) {
    change_t c = { idx, motion, available };
    if (HAPPlatformRunLoopScheduleCallback(Deliver, &c, sizeof c))
        HAPLogError(&logObject, "could not schedule motion update");
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
// With a yolo model rvd also says which classes are new or moving (class_mask, bit n = class n).
static int PollRvd(bool* hasMask, int* classMask) {
    char resp[512];
    *hasMask = false;
    *classMask = 0;
    if (rss_ctrl_send_command(RVD_SOCK, "{\"cmd\":\"ivs-status\"}", resp, sizeof resp, 500) <= 0)
        return -1;
    if (strstr(resp, "\"active\":false"))
        return -2;
    const char* m = strstr(resp, "\"class_mask\":");
    if (m) {
        *hasMask = true;
        *classMask = (int) strtol(m + 13, NULL, 10);
    }
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

// The debounce, per sensor:
//   on:  minHits raw hits within windowMs
//   off: no raw hit for holdMs (or the source went away)
typedef struct {
    uint64_t hits[16];
    unsigned nHits;
    uint64_t lastHit;
    bool reported;
} debounce_t;

static unsigned gMinHits, gWindowMs, gHoldMs;

static bool Debounce(debounce_t* d, bool hit, bool avail, uint64_t now) {
    if (hit) {
        d->lastHit = now;
        if (d->nHits == HAPArrayCount(d->hits)) {
            memmove(d->hits, d->hits + 1, sizeof d->hits - sizeof d->hits[0]);
            d->nHits--;
        }
        d->hits[d->nHits++] = now;
    }
    while (d->nHits && now - d->hits[0] > gWindowMs) {
        memmove(d->hits, d->hits + 1, (d->nHits - 1) * sizeof d->hits[0]);
        d->nHits--;
    }
    bool motion = d->reported;
    if (!d->reported && d->nHits >= gMinHits)
        motion = true;
    else if (d->reported && (!avail || now - d->lastHit >= gHoldMs))
        motion = false;
    return motion;
}

// Source: rvd's raw IVS flag (blips of one or two samples). rmd's state is not used for the sensor any more:
// its post-roll (record_post_sec, 30 s by default) is meant for recording and kept the sensor "detected" for
// most of the time on a busy scene. Debounce here instead:
//   on:  RHKD_MOTION_MIN_HITS raw hits (default 2) within RHKD_MOTION_WINDOW_MS (default 3000)
//   off: no raw hit for RHKD_MOTION_HOLD_SEC (default 10)
// rmd is only polled as a fallback source when rvd does not answer. With a yolo model the main sensor (the one
// HKSV is linked to) counts only the record classes, and each class sensor its own class.
static void* Thread(void* arg HAP_UNUSED) {
    gMinHits = EnvUint("RHKD_MOTION_MIN_HITS", 2);
    gWindowMs = EnvUint("RHKD_MOTION_WINDOW_MS", 3000);
    gHoldMs = EnvUint("RHKD_MOTION_HOLD_SEC", 10) * 1000;
    HAPLogInfo(&logObject, "motion: on after %u hits in %u ms, off after %u s quiet", gMinHits, gWindowMs,
               gHoldMs / 1000);

    debounce_t main = { 0 }, cls[RHKD_MAX_CLASS_SENSORS] = { 0 };
    bool lastAvail = false, lastClassAvail = false, first = true;
    const char* lastSource = "";
    for (;;) {
        const char* source = "rvd";
        bool hasMask;
        int classMask;
        int m = PollRvd(&hasMask, &classMask);
        bool classAvail = m >= 0;
        bool raw = m > 0;
        if (m == -2) {
            // Report the sensor as inactive (StatusActive false) instead of a sensor that never triggers.
            source = "off";
            m = -1;
        } else if (m < 0) {
            source = "rmd";
            m = PollRmd();
            raw = m > 0;
            hasMask = false;
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

        bool hit = classes_record_hit(gClasses, raw, hasMask, classMask);
        bool motion = Debounce(&main, hit, avail, now);
        if (first || motion != main.reported || avail != lastAvail) {
            atomic_store(&gMotion, motion);
            atomic_store(&gAvailable, avail);
            if (!first || motion || !avail)
                HAPLogInfo(&logObject, "motion %s%s", motion ? "detected" : "cleared", avail ? "" : " (no motion source)");
            Schedule(-1, motion, avail);
            main.reported = motion;
            lastAvail = avail;
        }

        atomic_store(&gClassAvailable, classAvail);
        for (int i = 0; i < gClasses->count; i++) {
            bool chit = raw && hasMask && (classMask & (1 << gClasses->id[i]));
            bool cm = Debounce(&cls[i], chit, classAvail, now);
            if (first || cm != cls[i].reported || classAvail != lastClassAvail) {
                atomic_store(&gClassMotion[i], cm);
                if (cm != cls[i].reported)
                    HAPLogInfo(&logObject, "%s %s", gClasses->name[i], cm ? "detected" : "cleared");
                Schedule(i, cm, classAvail);
                cls[i].reported = cm;
            }
        }
        lastClassAvail = classAvail;
        first = false;

        struct timespec ts = { gIntervalMs / 1000, (long) (gIntervalMs % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int motion_start(motion_changed_fn onChange, motion_class_changed_fn onClass, const classes_t* classes,
                 unsigned intervalMs) {
    gOnChange = onChange;
    gOnClass = onClass;
    gClasses = classes;
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

bool motion_class_detected(int idx) {
    return idx >= 0 && idx < RHKD_MAX_CLASS_SENSORS && atomic_load(&gClassMotion[idx]);
}

bool motion_class_available(void) {
    return atomic_load(&gClassAvailable);
}
