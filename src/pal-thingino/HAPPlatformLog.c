// Thingino PAL: logging.
//
// Replaces the ADK POSIX logger (ANSI colours on stderr). Messages go to syslog when RHKD_SYSLOG=1
// (the init script sets it), otherwise to stderr as plain text. The level is chosen at run time with
// RHKD_LOG_LEVEL (0 none, 1 default, 2 info, 3 debug; default 2), capped by the compile-time HAP_LOG_LEVEL.
//
// Based on HomeKitADK PAL/POSIX/HAPPlatformLog.c.
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>

#include "HAP.h"
#include "HAPPlatformLog+Init.h"

static const HAPLogObject logObject = { .subsystem = kHAPPlatform_LogSubsystem, .category = "Log" };

static int runtimeLevel = -1;
static bool useSyslog;

static void InitOnce(void) {
    if (runtimeLevel >= 0)
        return;
    const char* l = getenv("RHKD_LOG_LEVEL");
    int level = l && *l ? atoi(l) : 2;
    if (level < 0)
        level = 0;
    if (level > HAP_LOG_LEVEL)
        level = HAP_LOG_LEVEL;
    const char* s = getenv("RHKD_SYSLOG");
    useSyslog = s && *s == '1';
    if (useSyslog)
        openlog("rhkd", LOG_PID, LOG_DAEMON);
    runtimeLevel = level;
}

void HAPPlatformLogPOSIXError(
        HAPLogType type,
        const char* _Nonnull message,
        int errorNumber,
        const char* _Nonnull function,
        const char* _Nonnull file,
        int line) {
    HAPPrecondition(message);
    HAPPrecondition(function);
    HAPPrecondition(file);
    char errorString[256];
    if (strerror_r(errorNumber, errorString, sizeof errorString) != 0)
        snprintf(errorString, sizeof errorString, "Unknown error %d", errorNumber);
    HAPLogWithType(&logObject, type, "%s:%d:%s - %s @ %s:%d", message, errorNumber, errorString, function, file, line);
}

HAP_RESULT_USE_CHECK
HAPPlatformLogEnabledTypes HAPPlatformLogGetEnabledTypes(const HAPLogObject* _Nonnull log HAP_UNUSED) {
    InitOnce();
    switch (runtimeLevel) {
        case 0:
            return kHAPPlatformLogEnabledTypes_None;
        case 1:
            return kHAPPlatformLogEnabledTypes_Default;
        case 2:
            return kHAPPlatformLogEnabledTypes_Info;
        default:
            return kHAPPlatformLogEnabledTypes_Debug;
    }
}

void HAPPlatformLogCapture(
        const HAPLogObject* _Nonnull log,
        HAPLogType type,
        const char* _Nonnull message,
        const void* _Nullable bufferBytes,
        size_t numBufferBytes) HAP_DIAGNOSE_ERROR(!bufferBytes && numBufferBytes, "empty buffer cannot have a length") {
    HAPPrecondition(log);
    HAPPrecondition(message);
    HAPPrecondition(!numBufferBytes || bufferBytes);
    InitOnce();

    static volatile bool captureLock = 0;
    while (__atomic_test_and_set(&captureLock, __ATOMIC_SEQ_CST))
        ;

    const char* cat = log->category ? log->category : "";
    if (useSyslog) {
        int prio = LOG_INFO;
        switch (type) {
            case kHAPLogType_Debug: prio = LOG_DEBUG; break;
            case kHAPLogType_Info: prio = LOG_INFO; break;
            case kHAPLogType_Default: prio = LOG_NOTICE; break;
            case kHAPLogType_Error: prio = LOG_ERR; break;
            case kHAPLogType_Fault: prio = LOG_CRIT; break;
        }
        // Buffers (hex dumps) are debug-only noise in syslog: note their size instead.
        if (bufferBytes)
            syslog(prio, "[%s] %s (%zu bytes)", cat, message, numBufferBytes);
        else
            syslog(prio, "[%s] %s", cat, message);
    } else {
        static const char* names[] = { "Debug", "Info", "Default", "Error", "Fault" };
        int ti = 0;
        switch (type) {
            case kHAPLogType_Debug: ti = 0; break;
            case kHAPLogType_Info: ti = 1; break;
            case kHAPLogType_Default: ti = 2; break;
            case kHAPLogType_Error: ti = 3; break;
            case kHAPLogType_Fault: ti = 4; break;
        }
        struct timeval now;
        struct tm g;
        gettimeofday(&now, NULL);
        localtime_r(&now.tv_sec, &g);
        fprintf(stderr, "%02d:%02d:%02d.%03d %-7s [%s] %s\n", g.tm_hour, g.tm_min, g.tm_sec, (int)(now.tv_usec / 1000),
                names[ti], cat, message);
        if (bufferBytes && numBufferBytes) {
            const uint8_t* b = bufferBytes;
            for (size_t i = 0; i < numBufferBytes; i += 32) {
                fprintf(stderr, "    %04zx ", i);
                for (size_t n = 0; n < 32; n++) {
                    if (n % 4 == 0)
                        fputc(' ', stderr);
                    if (i + n < numBufferBytes)
                        fprintf(stderr, "%02x", b[i + n]);
                    else
                        fputs("  ", stderr);
                }
                fputs("    ", stderr);
                for (size_t n = 0; n < 32 && i + n < numBufferBytes; n++)
                    fputc(b[i + n] >= 32 && b[i + n] < 127 ? b[i + n] : '.', stderr);
                fputc('\n', stderr);
            }
        }
        fflush(stderr);
    }

    __atomic_clear(&captureLock, __ATOMIC_SEQ_CST);
}
